#include "longshot.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace longshot {

namespace {

// Offsets into game.dll.
constexpr uintptr_t kEnvironment = 0xe6080;
constexpr size_t kEnvironmentSun = 0x68;
constexpr size_t kEnvironmentClock = 0x84;   // seconds into the day
constexpr size_t kEnvironmentFrozen = 0x88;  // set, or its own tick adds ten times real time as well
constexpr size_t kEnvironmentLightLevel = 0x94;
constexpr size_t kSunAmbient = 0x24;
constexpr size_t kSunAmbientCount = 0x28;
constexpr size_t kSunSeen = 0xac;            // how much of the disc and halo get drawn
constexpr size_t kSunDazzle = 0xcc;
constexpr size_t kSunDazzleWanted = 0xd0;

// The game calls a hunt off at 21:00, which rules out hunting at night.
constexpr uintptr_t kCurfewCheck = 0x93ce3;
constexpr unsigned char kCurfewOn[] = {0x75, 0x2C};   // jne past the message
constexpr unsigned char kCurfewOff[] = {0xEB, 0x2C};  // jmp past it

// The game's two lists of hours are missing the commas after 21, so 21 to 23 are one long string,
// and the lists stop there. Past that is whatever's next in memory, which for 23:00 on the clock
// isn't text at all. It never got that far, with hunts ending at 21:00.
constexpr uintptr_t kHourNames = 0x1e57c0;
constexpr uintptr_t kHourNamesRead = 0x101ca7;   // inside the one lea that reads it
constexpr uintptr_t kClockNames = 0x1e5818;
constexpr uintptr_t kClockNamesRead = 0x101d07;

constexpr float kSecondsInDay = 86400.0f;
constexpr float kSecondsInHour = 3600.0f;
constexpr int kHoursInDay = 24;
constexpr double kLongestStep = 0.25;        // a hitch shouldn't jump the clock

// The game has no light at all from 22:00 to 4:00, so the moon stands in for it. It's only
// ambient, as the sun's direction doesn't go below 20 degrees and would light things from there.
constexpr float kMoonlight[3] = {0.13f, 0.15f, 0.22f};
constexpr float kSunGoesDown = 21.0f;
constexpr float kSunComesUp = 5.0f;
constexpr float kDuskStarts = 19.0f;         // how long it takes to get fully dark, or light
constexpr float kDawnEnds = 7.0f;

const char* const kHourText[kHoursInDay] = {
    "@@g_hour_00@@", "@@g_hour_01@@", "@@g_hour_02@@", "@@g_hour_03@@", "@@g_hour_04@@", "@@g_hour_05@@",
    "@@g_hour_06@@", "@@g_hour_07@@", "@@g_hour_08@@", "@@g_hour_09@@", "@@g_hour_10@@", "@@g_hour_11@@",
    "@@g_hour_12@@", "@@g_hour_13@@", "@@g_hour_14@@", "@@g_hour_15@@", "@@g_hour_16@@", "@@g_hour_17@@",
    "@@g_hour_18@@", "@@g_hour_19@@", "@@g_hour_20@@", "@@g_hour_21@@", "@@g_hour_22@@", "@@g_hour_23@@",
};
const char* const kClockText[kHoursInDay] = {
    "@@g_clock_00@@", "@@g_clock_01@@", "@@g_clock_02@@", "@@g_clock_03@@", "@@g_clock_04@@", "@@g_clock_05@@",
    "@@g_clock_06@@", "@@g_clock_07@@", "@@g_clock_08@@", "@@g_clock_09@@", "@@g_clock_10@@", "@@g_clock_11@@",
    "@@g_clock_12@@", "@@g_clock_13@@", "@@g_clock_14@@", "@@g_clock_15@@", "@@g_clock_16@@", "@@g_clock_17@@",
    "@@g_clock_18@@", "@@g_clock_19@@", "@@g_clock_20@@", "@@g_clock_21@@", "@@g_clock_22@@", "@@g_clock_23@@",
};

using EnvironmentFn = char*(__cdecl*)();

char* g_game = nullptr;
bool g_on_hunt = false;
double g_last_frame = 0.0;
float g_hour = -1.0f;
float g_light_level = 1.0f;

template <typename T>
T& field(void* object, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

char* environment() { return reinterpret_cast<EnvironmentFn>(g_game + kEnvironment)(); }

void set_curfew(bool on) {
    if (!g_game) return;
    const unsigned char* wanted = on ? kCurfewOn : kCurfewOff;
    const unsigned char* other = on ? kCurfewOff : kCurfewOn;
    if (memcmp(g_game + kCurfewCheck, wanted, sizeof(kCurfewOn)) == 0) return;
    if (!patch_bytes(g_game + kCurfewCheck, other, wanted, sizeof(kCurfewOn))) logf("daytime: couldn't change the 9 PM check");
}

bool use_list(uintptr_t read, uintptr_t game_list, const char* const* ours) {
    const void* was = g_game + game_list;
    const void* now = ours;
    return patch_bytes(g_game + read, &was, &now, sizeof(now));
}

float advanced_clock(float seconds, double real_seconds, float day_minutes) {
    float now = seconds + static_cast<float>(real_seconds) * kSecondsInDay / (day_minutes * 60.0f);
    while (now >= kSecondsInDay) now -= kSecondsInDay;
    return now;
}

bool sun_is_down(float hour) { return hour < kSunComesUp || hour >= kSunGoesDown; }

float darkness_at(float hour) {
    if (sun_is_down(hour)) return 1.0f;
    if (hour >= kDuskStarts) return (hour - kDuskStarts) / (kSunGoesDown - kDuskStarts);
    return hour < kDawnEnds ? (kDawnEnds - hour) / (kDawnEnds - kSunComesUp) : 0.0f;
}

void add_moonlight(float* levels, int hours, float brightness) {
    for (int hour = 0; hour < hours; ++hour) {
        float* level = levels + hour * 4;
        for (int part = 0; part < 3; ++part) {
            const float moon = kMoonlight[part] * brightness;
            if (level[part] < moon) level[part] = moon;
        }
    }
}

void update(double now) {
    char* place = environment();
    if (!place) return;
    char* sun = field<char*>(place, kEnvironmentSun);
    if (g_last_frame == 0.0 && sun && field<int>(sun, kSunAmbientCount) == kHoursInDay) {
        add_moonlight(field<float*>(sun, kSunAmbient), kHoursInDay, g_cfg.free_hunt_night_light);
    }
    const double step = g_last_frame > 0.0 ? now - g_last_frame : 0.0;
    g_last_frame = now;
    field<bool>(place, kEnvironmentFrozen) = true;
    float& clock = field<float>(place, kEnvironmentClock);
    clock = advanced_clock(clock, step > kLongestStep ? kLongestStep : step, g_cfg.free_hunt_day_minutes);
    g_hour = clock / kSecondsInHour;
    g_light_level = field<float>(place, kEnvironmentLightLevel);
}

bool update_safely(double now) {
    __try {
        update(now);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void daytime_hunt_started() {
    set_curfew(false);
    g_on_hunt = g_game != nullptr && g_cfg.free_hunt_day_minutes > 0.0f;
    g_last_frame = 0.0;
    g_hour = -1.0f;
}

void daytime_hunt_ended() {
    set_curfew(true);
    g_on_hunt = false;
    g_hour = -1.0f;
}

void daytime_end_frame() {
    if (!g_on_hunt) return;
    if (!walking_player()) {
        daytime_hunt_ended();
        return;
    }
    if (!view_hunt_running()) {
        g_last_frame = g_last_frame > 0.0 ? now_seconds() : 0.0;
        return;
    }
    if (update_safely(now_seconds())) return;
    daytime_hunt_ended();
    logf("daytime: something went wrong, so the clock stops for this hunt");
}

float daytime_hour() { return g_hour; }

float daytime_darkness() { return g_hour < 0.0f ? 0.0f : darkness_at(g_hour); }

float daytime_light_level() { return g_light_level; }

void daytime_before_render() {
    if (!g_on_hunt || g_hour < 0.0f || !sun_is_down(g_hour)) return;
    char* place = environment();
    char* sun = place ? field<char*>(place, kEnvironmentSun) : nullptr;
    if (!sun) return;
    field<float>(sun, kSunSeen) = 0.0f;
    field<float>(sun, kSunDazzle) = 0.0f;
    field<float>(sun, kSunDazzleWanted) = 0.0f;
}

void daytime_attach_game(HMODULE game, bool verified) {
    if (!verified) return;
    g_game = reinterpret_cast<char*>(game);
    const bool hours = use_list(kHourNamesRead, kHourNames, kHourText) && use_list(kClockNamesRead, kClockNames, kClockText);
    logf("daytime: a day takes %.0f minutes, hour names=%d", g_cfg.free_hunt_day_minutes, hours);
}

int daytime_test() {
    if (fabsf(advanced_clock(0.0f, 60.0, 24.0f) - 3600.0f) > 0.5f) return 1;     // a minute is an hour in a 24 minute day
    if (fabsf(advanced_clock(86000.0f, 60.0, 24.0f) - 3200.0f) > 0.5f) return 2;  // past midnight it starts again
    if (fabsf(advanced_clock(28800.0f, 0.0, 48.0f) - 28800.0f) > 0.01f) return 3;
    if (sun_is_down(12.0f) || sun_is_down(5.0f) || sun_is_down(20.9f)) return 4;
    if (!sun_is_down(21.0f) || !sun_is_down(23.9f) || !sun_is_down(4.9f)) return 5;

    float levels[kHoursInDay * 4] = {};
    levels[12 * 4] = 0.5f;
    levels[12 * 4 + 3] = 1.0f;
    add_moonlight(levels, kHoursInDay, 1.0f);
    if (levels[0] != kMoonlight[0] || levels[2] != kMoonlight[2]) return 6;       // night gets the moon
    if (levels[12 * 4] != 0.5f) return 7;                                         // and the day keeps what it has
    if (levels[12 * 4 + 1] != kMoonlight[1]) return 8;                            // though nothing goes below it
    if (levels[3] != 0.0f) return 9;                                              // nor is the last value touched

    if (darkness_at(12.0f) != 0.0f || darkness_at(7.0f) != 0.0f || darkness_at(19.0f) != 0.0f) return 10;
    if (darkness_at(23.0f) != 1.0f || darkness_at(3.0f) != 1.0f || darkness_at(21.0f) != 1.0f) return 11;
    if (darkness_at(20.0f) != 0.5f || darkness_at(6.0f) != 0.5f) return 12;       // and it comes on gradually

    for (int hour = 0; hour < kHoursInDay; ++hour) {
        char expected[32];
        snprintf(expected, sizeof(expected), "@@g_hour_%02d@@", hour);
        if (strcmp(kHourText[hour], expected) != 0) return 13;
        snprintf(expected, sizeof(expected), "@@g_clock_%02d@@", hour);
        if (strcmp(kClockText[hour], expected) != 0) return 14;
    }
    return 0;
}

}  // namespace longshot
