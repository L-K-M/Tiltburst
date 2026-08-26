// WinRawInputSource (05-engine-core.md §9.5): Windows Raw Input via a
// hidden message-only window on the dedicated raw-input thread.
#include "platform/input.h"
#include "platform/input_internal.h"

#if defined(_WIN32)

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core/log.h"
#include "core/time.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <thread>
#include <windows.h>

namespace tb::input {
namespace {

constexpr wchar_t kClassName[] = L"TiltburstRawInput";

// Scan code set 1 → SDL scancode for the keys Tiltburst binds. Left Shift =
// 0x2A, Right Shift = 0x36 — distinguished by MakeCode, NOT by E0 (§9.5).
struct Set1Entry {
    uint16_t make;
    uint16_t e0; // requires the E0 prefix when 1
    SDL_Scancode sc;
};

constexpr Set1Entry kSet1Map[] = {
    {0x02, 0, SDL_SCANCODE_1},     {0x03, 0, SDL_SCANCODE_2},     {0x04, 0, SDL_SCANCODE_3},
    {0x0B, 0, SDL_SCANCODE_0},     {0x10, 0, SDL_SCANCODE_Q},     {0x11, 0, SDL_SCANCODE_W},
    {0x12, 0, SDL_SCANCODE_E},     {0x13, 0, SDL_SCANCODE_R},     {0x14, 0, SDL_SCANCODE_T},
    {0x15, 0, SDL_SCANCODE_Y},     {0x16, 0, SDL_SCANCODE_U},     {0x17, 0, SDL_SCANCODE_I},
    {0x18, 0, SDL_SCANCODE_O},     {0x19, 0, SDL_SCANCODE_P},     {0x1E, 0, SDL_SCANCODE_A},
    {0x1F, 0, SDL_SCANCODE_S},     {0x20, 0, SDL_SCANCODE_D},     {0x21, 0, SDL_SCANCODE_F},
    {0x22, 0, SDL_SCANCODE_G},     {0x23, 0, SDL_SCANCODE_H},     {0x24, 0, SDL_SCANCODE_J},
    {0x25, 0, SDL_SCANCODE_K},     {0x26, 0, SDL_SCANCODE_L},     {0x2A, 0, SDL_SCANCODE_LSHIFT},
    {0x2C, 0, SDL_SCANCODE_Z},     {0x2D, 0, SDL_SCANCODE_X},     {0x2E, 0, SDL_SCANCODE_C},
    {0x2F, 0, SDL_SCANCODE_V},     {0x30, 0, SDL_SCANCODE_B},     {0x31, 0, SDL_SCANCODE_N},
    {0x32, 0, SDL_SCANCODE_M},     {0x33, 0, SDL_SCANCODE_COMMA}, {0x34, 0, SDL_SCANCODE_PERIOD},
    {0x35, 0, SDL_SCANCODE_SLASH}, {0x39, 0, SDL_SCANCODE_SPACE}, {0x1C, 0, SDL_SCANCODE_RETURN},
    {0x1D, 0, SDL_SCANCODE_LCTRL}, {0x1D, 1, SDL_SCANCODE_RCTRL}, {0x38, 0, SDL_SCANCODE_LALT},
    {0x38, 1, SDL_SCANCODE_RALT},  {0x48, 1, SDL_SCANCODE_UP},    {0x4B, 1, SDL_SCANCODE_LEFT},
    {0x4D, 1, SDL_SCANCODE_RIGHT}, {0x50, 1, SDL_SCANCODE_DOWN},
};

SDL_Scancode map_set1_to_sdl(uint32_t sc) {
    const bool e0 = (sc & 0xE000) != 0;
    const uint16_t make = uint16_t(sc & 0xFF);
    for (const Set1Entry& entry : kSet1Map) {
        if (entry.make == make && (!e0 || entry.e0 != 0)) {
            return entry.sc;
        }
    }
    return SDL_SCANCODE_UNKNOWN;
}

class WinRawInputSource final : public RingSource {
public:
    const char* name() const override { return "winraw"; }

    bool start() override {
        run_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { thread_body(); });
        for (int i = 0; i < 100 && !started_.load(std::memory_order_acquire); ++i) {
            Sleep(10);
        }
        if (!ok_.load(std::memory_order_acquire)) {
            stop();
            return false; // → SDL fallback (§9.8)
        }
        return true;
    }

    void stop() override {
        if (!run_.exchange(false, std::memory_order_acq_rel)) {
            return; // idempotent
        }
        if (hwnd_ != nullptr) {
            PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    static LRESULT CALLBACK raw_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_INPUT) {
            RAWINPUT ri;
            UINT size = sizeof(ri);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam),
                                RID_INPUT,
                                &ri,
                                &size,
                                sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEKEYBOARD && self_ != nullptr) {
                self_->on_raw_keyboard(ri.data.keyboard);
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            PostQuitMessage(0); // §9.5 stop path
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void on_raw_keyboard(const RAWKEYBOARD& kb) {
        if (kb.MakeCode == 0) {
            return; // synthetic, ignore (§9.5)
        }
        const bool release = (kb.Flags & RI_KEY_BREAK) != 0;
        uint32_t sc = kb.MakeCode;
        if (kb.Flags & RI_KEY_E0) {
            sc |= 0xE000;
        }
        // Raw input repeats 'make' while held: edge-filter with a local
        // bitset (all WM_INPUT arrives on this thread — plain state is fine).
        const size_t slot = (sc >> 6) & 1u;
        const uint64_t bit = uint64_t(1u) << (sc & 0x3F);
        if (release) {
            if ((held_[slot] & bit) == 0) {
                return;
            }
            held_[slot] &= ~bit;
        } else {
            if ((held_[slot] & bit) != 0) {
                return;
            }
            held_[slot] |= bit;
        }
        const SDL_Scancode sdl_sc = map_set1_to_sdl(sc);
        if (sdl_sc == SDL_SCANCODE_UNKNOWN) {
            return;
        }
        push_actions(sdl_sc, !release);
    }

    void push_actions(SDL_Scancode sdl_sc, bool pressed) {
        const Keymap* map = active_keymap();
        if (map == nullptr) {
            return;
        }
        const uint32_t actions = map->actions_for(static_cast<int>(sdl_sc));
        for (uint16_t action = 0; action < kActionCount; ++action) {
            if (((actions >> action) & 1u) == 0u) {
                continue;
            }
            submit(
                InputEdge{tb_now_ns(), action, pressed ? uint8_t(1) : uint8_t(0), kSourceWinRaw});
        }
    }

    void thread_body() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &WinRawInputSource::raw_wnd_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            fail();
            return;
        }
        hwnd_ = CreateWindowExW(
            0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (hwnd_ == nullptr) {
            fail();
            return;
        }
        self_ = this;

        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;        // Generic Desktop
        rid.usUsage = 0x06;            // Keyboard
        rid.dwFlags = RIDEV_INPUTSINK; // receive without focus (§9.5)
        rid.hwndTarget = hwnd_;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            TB_LOG_WARN(
                "input", "RegisterRawInputDevices failed: {}", static_cast<int>(GetLastError()));
            fail();
            return;
        }

        ok_.store(true, std::memory_order_release);
        started_.store(true, std::memory_order_release);

        MSG msg;
        while (run_.load(std::memory_order_acquire) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        UnregisterClassW(kClassName, wc.hInstance);
        self_ = nullptr;
    }

    void fail() {
        ok_.store(false, std::memory_order_release);
        started_.store(true, std::memory_order_release);
    }

    static inline WinRawInputSource* self_ = nullptr;

    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> ok_{false};
    uint64_t held_[2] = {0, 0}; // edge filter; raw thread only
    HWND hwnd_ = nullptr;
};

} // namespace

std::unique_ptr<InputSource> make_winraw_input_source() {
    return std::make_unique<WinRawInputSource>();
}

} // namespace tb::input

#else // !_WIN32

#include "platform/input.h"

namespace tb::input {
std::unique_ptr<InputSource> make_winraw_input_source() {
    return nullptr; // §9.7/§9.8: Windows only
}
} // namespace tb::input

#endif
