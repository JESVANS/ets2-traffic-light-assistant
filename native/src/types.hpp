#pragma once

#include <cstdint>
#include <string>

namespace tlhud {

#pragma pack(push, 1)
struct CameraMemData {
    float fov;
    float pos_x;
    float pos_y;
    float pos_z;
    int16_t cx;
    int16_t cz;
    float qw;
    float qx;
    float qy;
    float qz;
    float m[16];         // m11..m44 row-major projection matrix
    float truck_pos_x;   // global world X
    float truck_pos_y;   // global world Y
    float truck_pos_z;   // global world Z
    float truck_rot_w;
    float truck_rot_x;
    float truck_rot_y;
    float truck_rot_z;
};
static_assert(sizeof(CameraMemData) == 128, "CameraMemData must be 128 bytes");

struct SemaphoreObject {
    float x;
    float y;
    float z;
    int16_t cx;
    int16_t cz;
    float qw;
    float qx;
    float qy;
    float qz;
    int32_t type;           // 1 = traffic light, 2 = gate
    float time_remaining;
    int32_t state;
    int32_t id;
};
static_assert(sizeof(SemaphoreObject) == 48, "SemaphoreObject must be 48 bytes");

struct SemaphoreMemData {
    SemaphoreObject items[40];
};
static_assert(sizeof(SemaphoreMemData) == 1920, "SemaphoreMemData must be 1920 bytes");
#pragma pack(pop)

constexpr int32_t TYPE_TRAFFIC_LIGHT = 1;
constexpr int32_t TYPE_GATE = 2;

enum class Phase {
    Unknown,
    Off,
    Red,
    RedYellow,
    Green,
    Yellow,
    Flashing,
    GateClosed,
    GateClosing,
    GateOpening,
    GateOpen,
};

enum class AdviceLevel {
    Go,
    Adjust,
    Stop,
    Warn,
    Info,
};

struct TruckTelemetry {
    bool paused = true;
    bool on_world = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double heading = 0.0; // radians, 0 = -Z (North)
    float speed = 0.0f;   // m/s
    float speed_limit = 0.0f;  // m/s
    float cruise_speed = 0.0f; // m/s
    uint64_t last_update_ms = 0;
};

struct SemaphoreParsed {
    int index = 0;
    int32_t type = 0;
    int32_t state = 0;
    Phase phase = Phase::Unknown;
    float time_left = 0.0f;
    int32_t id = 0;
    double wx = 0.0;
    double wy = 0.0;
    double wz = 0.0;
    float qw = 1.0f;
    float qx = 0.0f;
    float qy = 0.0f;
    float qz = 0.0f;
};

struct TargetResult {
    SemaphoreParsed sem;
    float distance = 0.0f;     // longitudinal distance to stop line (m)
    float longitudinal = 0.0f; // longitudinal distance to pole (m)
    float lateral = 0.0f;      // lateral offset (m)
};

struct AdviceResult {
    AdviceLevel level = AdviceLevel::Info;
    std::string text; // UTF-8
    float target_kmh = 0.0f;
    bool has_target_kmh = false;
};

struct HudSnapshot {
    bool visible = false;
    Phase phase = Phase::Unknown;
    bool is_gate = false;
    float time_left = 0.0f;
    float distance = 0.0f;
    AdviceLevel advice_level = AdviceLevel::Info;
    std::string advice_text; // UTF-8
    std::string toast_text;  // UTF-8, non-empty during hotkey feedback
    bool is_demo = false;
};

struct HudConfig {
    std::string mode = "window_3d"; // "window_3d" or "screen_2d"
    float offset_x = 0.160f;
    float offset_y = -0.245f;
    float offset_z = -1.390f;
    float panel_width = 0.420f;
    float panel_height = 0.155f;
    float tilt_deg = 8.0f;
    float screen_x = 0.560f;
    float screen_y = 0.535f;
    float screen_width = 0.210f;
    float bg_opacity = 0.28f;
    float hud_alpha = 0.94f;
    bool hide_outside_cab = true;
    bool sound_alert = true;
    bool start_demo = false;
};

struct SourceConfig {
    float sector_size = 512.0f;
    float stale_timeout = 3.0f;
};

struct SelectionConfig {
    float max_distance = 300.0f;
    float max_lateral = 12.0f;
    float lateral_growth = 0.12f;
    float max_height_diff = 12.0f;
    float parallel_threshold = 0.6f;
    int facing_mode = 0;
    float stop_line_offset = 4.0f;
    float lateral_weight = 3.0f;
    bool include_gates = true;
};

struct AdvisorConfig {
    float comfort_decel = 2.0f;
    float max_decel = 4.5f;
    float max_accel = 0.6f;
    float min_glide_speed_kmh = 20.0f;
    float default_max_speed_kmh = 90.0f;
    float red_yellow_duration = 2.0f;
    bool use_speed_limit = true;
};

struct StateCodesConfig {
    int off = 0;
    int yellow = 1;
    int red = 2;
    int red_yellow = 4;
    int green = 8;
    int flashing = 32;
    int gate_closing = 0;
    int gate_closed = 1;
    int gate_opening = 2;
    int gate_open = 3;
};

struct PluginConfig {
    HudConfig hud;
    SourceConfig source;
    SelectionConfig selection;
    AdvisorConfig advisor;
    StateCodesConfig states;
};

} // namespace tlhud
