// EvdevInputSource (05-engine-core.md §9.6): Linux raw keyboard input on a
// dedicated thread; falls back to SDL when devices are unavailable.
#include "platform/input.h"
#include "platform/input_internal.h"

#if defined(__linux__)

#include "core/log.h"
#include "core/time.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <string>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace tb::input {
namespace {

constexpr size_t kMaxDevices = 64;

// KEY_* → SDL scancode for the keys Tiltburst binds (§9.6 map).
SDL_Scancode map_keycode_to_sdl(uint16_t code) {
    switch (code) {
    case KEY_1:
        return SDL_SCANCODE_1;
    case KEY_2:
        return SDL_SCANCODE_2;
    case KEY_3:
        return SDL_SCANCODE_3;
    case KEY_0:
        return SDL_SCANCODE_0;
    case KEY_Q:
        return SDL_SCANCODE_Q;
    case KEY_W:
        return SDL_SCANCODE_W;
    case KEY_E:
        return SDL_SCANCODE_E;
    case KEY_R:
        return SDL_SCANCODE_R;
    case KEY_T:
        return SDL_SCANCODE_T;
    case KEY_Y:
        return SDL_SCANCODE_Y;
    case KEY_U:
        return SDL_SCANCODE_U;
    case KEY_I:
        return SDL_SCANCODE_I;
    case KEY_O:
        return SDL_SCANCODE_O;
    case KEY_P:
        return SDL_SCANCODE_P;
    case KEY_A:
        return SDL_SCANCODE_A;
    case KEY_S:
        return SDL_SCANCODE_S;
    case KEY_D:
        return SDL_SCANCODE_D;
    case KEY_F:
        return SDL_SCANCODE_F;
    case KEY_G:
        return SDL_SCANCODE_G;
    case KEY_H:
        return SDL_SCANCODE_H;
    case KEY_J:
        return SDL_SCANCODE_J;
    case KEY_K:
        return SDL_SCANCODE_K;
    case KEY_L:
        return SDL_SCANCODE_L;
    case KEY_Z:
        return SDL_SCANCODE_Z;
    case KEY_X:
        return SDL_SCANCODE_X;
    case KEY_C:
        return SDL_SCANCODE_C;
    case KEY_V:
        return SDL_SCANCODE_V;
    case KEY_B:
        return SDL_SCANCODE_B;
    case KEY_N:
        return SDL_SCANCODE_N;
    case KEY_M:
        return SDL_SCANCODE_M;
    case KEY_COMMA:
        return SDL_SCANCODE_COMMA;
    case KEY_DOT:
        return SDL_SCANCODE_PERIOD;
    case KEY_SLASH:
        return SDL_SCANCODE_SLASH;
    case KEY_SPACE:
        return SDL_SCANCODE_SPACE;
    case KEY_ENTER:
        return SDL_SCANCODE_RETURN;
    case KEY_KPENTER:
        return SDL_SCANCODE_RETURN;
    case KEY_LEFTSHIFT:
        return SDL_SCANCODE_LSHIFT;
    case KEY_RIGHTSHIFT:
        return SDL_SCANCODE_RSHIFT;
    case KEY_LEFTCTRL:
        return SDL_SCANCODE_LCTRL;
    case KEY_RIGHTCTRL:
        return SDL_SCANCODE_RCTRL;
    case KEY_LEFTALT:
        return SDL_SCANCODE_LALT;
    case KEY_RIGHTALT:
        return SDL_SCANCODE_RALT;
    case KEY_UP:
        return SDL_SCANCODE_UP;
    case KEY_LEFT:
        return SDL_SCANCODE_LEFT;
    case KEY_RIGHT:
        return SDL_SCANCODE_RIGHT;
    case KEY_DOWN:
        return SDL_SCANCODE_DOWN;
    default:
        return SDL_SCANCODE_UNKNOWN;
    }
}

bool test_bit(const unsigned long* bits, unsigned int bit) {
    return (bits[bit / (8 * sizeof(unsigned long))] >> (bit % (8 * sizeof(unsigned long)))) & 1u;
}

class EvdevInputSource final : public RingSource {
public:
    const char* name() const override { return "evdev"; }

    bool start() override {
        run_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { thread_body(); });
        for (int i = 0; i < 100 && !started_.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!ok_.load(std::memory_order_acquire)) {
            stop();
            return false; // → SDL fallback (§9.8)
        }
        return true;
    }

    void stop() override {
        if (!run_.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (wake_pipe_[1] >= 0) {
            const char b = 1;
            ssize_t rc = write(wake_pipe_[1], &b, 1);
            (void)rc;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void thread_body() {
        scan_devices();
        open_inotify();

        ok_.store(true, std::memory_order_release); // started; may hold 0 fds
        started_.store(true, std::memory_order_release);

        struct pollfd polls[kMaxDevices + 2];
        while (run_.load(std::memory_order_acquire)) {
            nfds_t n = 0;
            for (int fd : device_fds_) {
                polls[n].fd = fd;
                polls[n].events = POLLIN;
                polls[n].revents = 0;
                ++n;
            }
            if (inotify_fd_ >= 0) {
                polls[n].fd = inotify_fd_;
                polls[n].events = POLLIN;
                polls[n].revents = 0;
                ++n;
            }
            polls[n].fd = wake_pipe_[0];
            polls[n].events = POLLIN;
            polls[n].revents = 0;
            ++n;

            // §9.6: 500 ms timeout so the stop flag is always observed.
            const int ready = ::poll(polls, n, 500);
            if (ready <= 0) {
                continue;
            }
            for (nfds_t i = 0; i + 1 < n; ++i) {
                if ((polls[i].revents & POLLIN) == 0) {
                    continue;
                }
                if (inotify_fd_ >= 0 && static_cast<int>(polls[i].fd) == inotify_fd_) {
                    drain_inotify();
                    continue;
                }
                read_device(static_cast<int>(polls[i].fd));
            }
            // Wake pipe drained implicitly next round.
        }

        for (int fd : device_fds_) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
        device_fds_.clear();
        if (inotify_fd_ >= 0) {
            ::close(inotify_fd_);
            inotify_fd_ = -1;
        }
    }

    // §9.6 discovery: sorted /dev/input/event*, keyboard-like only.
    void scan_devices() {
        DIR* dir = opendir("/dev/input");
        if (dir == nullptr) {
            return;
        }
        std::vector<std::string> names;
        while (const dirent* e = readdir(dir)) {
            const std::string name = e->d_name;
            if (name.rfind("event", 0) == 0) {
                names.push_back(name);
            }
        }
        closedir(dir);
        std::sort(names.begin(), names.end());

        for (const std::string& name : names) {
            if (device_fds_.size() >= kMaxDevices) {
                break;
            }
            const std::string path = "/dev/input/" + name;
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                if (errno == EACCES) {
                    saw_permission_denied_ = true;
                }
                continue;
            }
            unsigned long evbits[EV_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
            unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
            if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 || !test_bit(evbits, EV_KEY)) {
                close(fd);
                continue; // not a keyboard-like device
            }
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0 ||
                !(test_bit(keybits, KEY_LEFTSHIFT) || test_bit(keybits, KEY_ENTER))) {
                close(fd);
                continue; // skips mice and power buttons
            }
            int clk = CLOCK_MONOTONIC;
            ioctl(fd, EVIOCSCLOCKID, &clk);
            device_fds_.push_back(fd);
        }

        if (device_fds_.empty()) {
            ok_.store(false, std::memory_order_release);
            if (saw_permission_denied_) {
                // §9.6 exact fallback text.
                std::fprintf(stderr,
                             "WARNING: raw keyboard input unavailable: permission denied "
                             "opening /dev/input/event* devices.\n"
                             "Tiltburst falls back to SDL input (adds a few ms of latency).\n"
                             "To enable low-latency input, add your user to the 'input' group "
                             "and log out and back in:\n"
                             "    sudo usermod -aG input $USER\n");
                TB_LOG_WARN("input",
                            "raw keyboard input unavailable: permission denied opening "
                            "/dev/input/event* devices; falling back to SDL input");
            } else {
                TB_LOG_WARN("input", "no evdev keyboards found; falling back to SDL input");
            }
        } else {
            TB_LOG_INFO("input", "evdev: {} keyboard device(s)", device_fds_.size());
        }
    }

    void open_inotify() {
        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            return;
        }
        watch_fd_ = inotify_add_watch(inotify_fd_, "/dev/input", IN_CREATE | IN_DELETE);
    }

    void drain_inotify() {
        char buf[1024];
        while (true) {
            const ssize_t n = read(inotify_fd_, buf, sizeof(buf));
            if (n <= 0) {
                break;
            }
            // Replug support (§9.6): rescan on any create event.
            bool created = false;
            for (ssize_t off = 0; off + static_cast<ssize_t>(sizeof(struct inotify_event)) <= n;) {
                const auto* ev = reinterpret_cast<const struct inotify_event*>(buf + off);
                if ((ev->mask & IN_CREATE) != 0) {
                    created = true;
                }
                off += static_cast<ssize_t>(sizeof(struct inotify_event)) + ev->len;
            }
            if (created && device_fds_.size() < kMaxDevices) {
                rescan_locked();
            }
        }
    }

    void rescan_locked() {
        // Simple approach: close nothing, just try opening missing devices
        // by rescanning the directory (duplicates avoided by fd reuse of
        // the same path is impossible — track opened paths).
        DIR* dir = opendir("/dev/input");
        if (dir == nullptr) {
            return;
        }
        std::vector<std::string> names;
        while (const dirent* e = readdir(dir)) {
            const std::string name = e->d_name;
            if (name.rfind("event", 0) == 0) {
                names.push_back(name);
            }
        }
        closedir(dir);
        std::sort(names.begin(), names.end());
        for (const std::string& name : names) {
            if (device_fds_.size() >= kMaxDevices) {
                return;
            }
            const std::string path = "/dev/input/" + name;
            bool already = false;
            for (const std::string& p : opened_paths_) {
                if (p == path) {
                    already = true;
                    break;
                }
            }
            if (already) {
                continue;
            }
            const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }
            unsigned long evbits[EV_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
            unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {0};
            if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 || !test_bit(evbits, EV_KEY) ||
                ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0 ||
                !(test_bit(keybits, KEY_LEFTSHIFT) || test_bit(keybits, KEY_ENTER))) {
                close(fd);
                continue;
            }
            int clk = CLOCK_MONOTONIC;
            ioctl(fd, EVIOCSCLOCKID, &clk);
            device_fds_.push_back(fd);
            opened_paths_.push_back(path);
            TB_LOG_INFO("input", "evdev: device attached {}", path);
        }
        if (!device_fds_.empty()) {
            ok_.store(true, std::memory_order_release);
        }
    }

    void read_device(int fd) {
        input_event events[32];
        while (run_.load(std::memory_order_acquire)) {
            const ssize_t n = read(fd, events, sizeof(events));
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return;
                }
                if (errno == ENODEV) {
                    unplug_device(fd); // §9.6 device loss
                }
                return;
            }
            const size_t count = size_t(n) / sizeof(input_event);
            for (size_t i = 0; i < count; ++i) {
                const input_event& ev = events[i];
                // value: 0 release, 1 press, 2 autorepeat (dropped).
                if (ev.type != EV_KEY || ev.value == 2) {
                    continue;
                }
                const SDL_Scancode sdl_sc = map_keycode_to_sdl(ev.code);
                if (sdl_sc == SDL_SCANCODE_UNKNOWN) {
                    continue;
                }
                push_actions(sdl_sc, ev.value == 1, ev.time);
            }
        }
    }

    static uint64_t ev_time_to_monotonic_ns(const timeval& tv) {
        // Device clock switched to CLOCK_MONOTONIC via EVIOCSCLOCKID.
        return uint64_t(tv.tv_sec) * 1000000000ull + uint64_t(tv.tv_usec) * 1000ull;
    }

    void push_actions(SDL_Scancode sdl_sc, bool pressed, const timeval& tv) {
        const Keymap* map = active_keymap();
        if (map == nullptr) {
            return;
        }
        const uint32_t actions = map->actions_for(static_cast<int>(sdl_sc));
        for (uint16_t action = 0; action < kActionCount; ++action) {
            if (((actions >> action) & 1u) == 0u) {
                continue;
            }
            submit(InputEdge{ev_time_to_monotonic_ns(tv),
                             action,
                             pressed ? uint8_t(1) : uint8_t(0),
                             kSourceEvdev});
        }
    }

    void unplug_device(int fd) {
        TB_LOG_WARN("input", "evdev device lost (fd {})", fd);
        close(fd);
        for (size_t i = 0; i < device_fds_.size(); ++i) {
            if (device_fds_[i] == fd) {
                device_fds_.erase(device_fds_.begin() + ssize_t(i));
                if (i < opened_paths_.size()) {
                    opened_paths_.erase(opened_paths_.begin() + ssize_t(i));
                }
                break;
            }
        }
        if (device_fds_.empty()) {
            ok_.store(false, std::memory_order_release); // → SDL fallback (§9.8)
        }
    }

    std::vector<int> device_fds_;
    std::vector<std::string> opened_paths_;
    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> started_{false};
    std::atomic<bool> ok_{false};
    bool saw_permission_denied_ = false;
    int inotify_fd_ = -1;
    int watch_fd_ = -1;
    int wake_pipe_[2] = {-1, -1};
};

} // namespace

std::unique_ptr<InputSource> make_evdev_input_source() {
    return std::make_unique<EvdevInputSource>();
}

} // namespace tb::input

#else // !__linux__

#include "platform/input.h"

namespace tb::input {
std::unique_ptr<InputSource> make_evdev_input_source() {
    return nullptr; // Linux only (§9.7/§9.8)
}
} // namespace tb::input

#endif
