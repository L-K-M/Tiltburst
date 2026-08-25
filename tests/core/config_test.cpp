#include "core/config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path temp_file(const char* name) {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "tiltburst_tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

// §11: save → load reproduces every field; unknown keys in the file
// survive the round trip (§11.1).
TEST(unit_config, round_trip) {
    const auto path = temp_file("settings_roundtrip.json");

    tb::Settings s = tb::Settings::defaults();
    s.max_fps = -1;
    s.present_mode = "mailbox";
    s.brightness = 1.25f;
    s.audio_music = 33;
    s.nudge_level = 3;
    s.balls_per_game = 5;
    s.last_table = "test-lab";
    s.screen_shake = false;

    ASSERT_TRUE(s.save(path));
    tb::Settings loaded = tb::Settings::load(path);

    EXPECT_EQ(loaded.max_fps, -1);
    EXPECT_EQ(loaded.present_mode, "mailbox");
    EXPECT_NEAR(loaded.brightness, 1.25f, 1e-6f);
    EXPECT_EQ(loaded.audio_music, 33);
    EXPECT_EQ(loaded.nudge_level, 3);
    EXPECT_EQ(loaded.balls_per_game, 5);
    EXPECT_EQ(loaded.last_table, "test-lab");
    EXPECT_FALSE(loaded.screen_shake);
    EXPECT_EQ(loaded.bindings[0], std::vector<std::string>{"Left Shift"});

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(unit_config, unknown_keys_preserved) {
    const auto path = temp_file("settings_unknown.json");
    write_text(path,
               "{\n"
               "  \"future_thing\": {\"a\": 42},   // comments allowed\n"
               "  \"video\": {\"max_fps\": 90}\n"
               "}\n");

    tb::Settings s = tb::Settings::load(path);
    ASSERT_TRUE(s.save(path));

    const std::string after = read_text(path);
    EXPECT_NE(after.find("future_thing"), std::string::npos);
    EXPECT_NE(after.find("\"max_fps\": 90"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

TEST(unit_config, out_of_range_clamped) {
    const auto path = temp_file("settings_clamp.json");
    write_text(path,
               "{"
               "  \"video\": {\"max_fps\": 5000, \"brightness\": 9.0},"
               "  \"game\": {\"balls_per_game\": 7}"
               "}\n");

    tb::Settings s = tb::Settings::load(path);
    EXPECT_EQ(s.max_fps, 1000); // clamped into [30, 1000]
    EXPECT_NEAR(s.brightness, 1.5f, 1e-6f);
    EXPECT_EQ(s.balls_per_game, 3); // domain {3, 5} ⇒ 3 + warn

    std::error_code ec;
    std::filesystem::remove(path, ec);
}
