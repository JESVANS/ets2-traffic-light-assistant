#include <windows.h>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.hpp"
#include "hud_math.hpp"
#include "hud_painter.hpp"
#include "logic.hpp"
#include "types.hpp"

using namespace tlhud;

static void TestStructs() {
    assert(sizeof(CameraMemData) == 128);
    assert(sizeof(SemaphoreObject) == 48);
    assert(sizeof(SemaphoreMemData) == 1920);
    std::printf("[PASS] Struct sizes: Camera=128, Semaphore=48, SemaphoreMem=1920\n");
}

static void TestLogic() {
    PluginConfig cfg;
    SemaphoreMemData mem{};

    // 0: Red light ahead 100m (world 514, 10, -100)
    mem.items[0] = {2.0f, 10.0f, 412.0f, 1, -1, 1.0f, 0.0f, 0.0f, 0.0f, TYPE_TRAFFIC_LIGHT, 12.0f, cfg.states.red, 101};
    // 1: Cross-street light rotated 90 deg
    float c45 = std::cos(3.14159265f / 4.0f);
    float s45 = std::sin(3.14159265f / 4.0f);
    mem.items[1] = {20.0f, 10.0f, 420.0f, 1, -1, c45, 0.0f, s45, 0.0f, TYPE_TRAFFIC_LIGHT, 5.0f, cfg.states.green, 102};
    // 2: Behind truck (world 514, 10, +100)
    mem.items[2] = {2.0f, 10.0f, 100.0f, 1, 0, 1.0f, 0.0f, 0.0f, 0.0f, TYPE_TRAFFIC_LIGHT, 5.0f, cfg.states.green, 103};

    auto sems = ParseSemaphores(mem, cfg.source, cfg.states);
    assert(sems.size() == 3);

    TruckTelemetry truck;
    truck.paused = false;
    truck.on_world = true;
    truck.x = 512.0;
    truck.y = 10.0;
    truck.z = 0.0;
    truck.heading = 0.0; // Facing North (-Z)
    truck.speed = 50.0f / 3.6f;
    truck.speed_limit = 60.0f / 3.6f;

    TargetSelector sel;
    TargetResult tgt;
    assert(sel.Select(truck, sems, cfg.selection, tgt));
    assert(tgt.sem.id == 101);
    assert(std::fabs(tgt.distance - 96.0f) < 0.1f);

    AdviceResult adv = ComputeAdvice(tgt, truck, cfg.advisor);
    assert(adv.level == AdviceLevel::Adjust);
    assert(adv.has_target_kmh);
    std::printf("[PASS] Semaphore selection & advisor: id=101 dist=%.1f -> %s\n", tgt.distance, adv.text.c_str());

    // Hysteresis test: add competing light at identical score -> should stick to 101
    mem.items[3] = {-2.0f, 10.0f, 412.0f, 1, -1, 1.0f, 0.0f, 0.0f, 0.0f, TYPE_TRAFFIC_LIGHT, 8.0f, cfg.states.green, 104};
    sems = ParseSemaphores(mem, cfg.source, cfg.states);
    assert(sel.Select(truck, sems, cfg.selection, tgt));
    assert(tgt.sem.id == 101);
    std::printf("[PASS] Target hysteresis preserved id=101\n");
}

static CameraMemData MakeCamera(float yaw_deg, float pitch_deg, Vec3f truck_local_cam_pos, bool outside = false) {
    CameraMemData c{};
    c.fov = 65.0f;
    c.cx = 10;
    c.cz = -20;
    c.truck_pos_x = 10 * 512.0f + 100.0f;
    c.truck_pos_y = 20.0f;
    c.truck_pos_z = -20 * 512.0f + 200.0f;
    c.truck_rot_w = 1.0f; // identity truck orientation facing -Z

    Vec3f off = outside ? Vec3f{0.0f, 5.0f, 10.0f} : truck_local_cam_pos;
    c.pos_x = 100.0f + off.x;
    c.pos_y = 20.0f + off.y;
    c.pos_z = 200.0f + off.z;

    // Build camera rotation: yaw around Y, pitch around X
    float cy = std::cos(yaw_deg * 0.5f * 3.14159265f / 180.0f);
    float sy = std::sin(yaw_deg * 0.5f * 3.14159265f / 180.0f);
    float cp = std::cos(pitch_deg * 0.5f * 3.14159265f / 180.0f);
    float sp = std::sin(pitch_deg * 0.5f * 3.14159265f / 180.0f);
    // q = q_yaw * q_pitch
    c.qw = cy * cp;
    c.qx = cy * sp;
    c.qy = sy * cp;
    c.qz = -sy * sp;

    // Build row-major perspective projection matrix (16:9, hfov=65)
    float aspect = 16.0f / 9.0f;
    float fx = 1.0f / std::tan(c.fov * 0.5f * 3.14159265f / 180.0f);
    float fy = fx * aspect;
    c.m[0] = fx;
    c.m[5] = fy;
    c.m[10] = -1.0f;
    c.m[11] = -0.1f;
    c.m[14] = -1.0f;
    return c;
}

static void Test3DProjection() {
    HudConfig cfg; // default green-box offsets: +0.160, -0.245, -1.390
    CabAnchorTracker tracker;
    Vec3f eye{-0.45f, 1.55f, -0.80f}; // typical LHD cab seat eye in truck local space

    // 1. Inside cab, looking forward with typical -7 deg downward pitch
    CameraMemData cam_fwd = MakeCamera(0.0f, -7.0f, eye, false);
    bool in_cab = false;
    assert(tracker.Update(cam_fwd, 512.0, 1000, in_cab));
    assert(in_cab && tracker.HasAnchor());

    HudVertex v[4]{};
    assert(ProjectHudQuad(cam_fwd, tracker.Anchor(), cfg, 512.0, 1920.0f, 1080.0f, 0.94f, v));
    for (int i = 0; i < 4; ++i) assert(v[i].w > 0.5f);

    float sx0 = (v[0].x / v[0].w + 1.0f) * 0.5f; // TL X
    float sy0 = (1.0f - v[0].y / v[0].w) * 0.5f; // TL Y
    float sx1 = (v[1].x / v[1].w + 1.0f) * 0.5f; // TR X
    float sy2 = (1.0f - v[2].y / v[2].w) * 0.5f; // BL Y
    float center_x = 0.5f * (sx0 + sx1);
    float center_y = 0.5f * (sy0 + sy2);
    std::printf("[PASS] Forward cab HUD NDC->Screen: center=(%.3f, %.3f) width=%.3f height=%.3f\n",
                center_x, center_y, sx1 - sx0, sy2 - sy0);
    assert(sx1 > sx0 && sy2 > sy0);
    assert(center_x > 0.50f && center_x < 0.65f);
    assert(center_y > 0.45f && center_y < 0.65f);

    // 2. Turn head 30 deg right (yaw = -30 deg around +Y) -> windshield HUD shifts left on screen
    CameraMemData cam_right = MakeCamera(-30.0f, -7.0f, eye, false);
    assert(tracker.Update(cam_right, 512.0, 1200, in_cab));
    assert(in_cab);
    HudVertex vr[4]{};
    assert(ProjectHudQuad(cam_right, tracker.Anchor(), cfg, 512.0, 1920.0f, 1080.0f, 0.94f, vr));
    float rx0 = (vr[0].x / vr[0].w + 1.0f) * 0.5f;
    float rx1 = (vr[1].x / vr[1].w + 1.0f) * 0.5f;
    float rcx = 0.5f * (rx0 + rx1);
    std::printf("[PASS] Head turned right 30 deg -> HUD shifted left to center_x=%.3f (w_TL=%.3f, w_TR=%.3f)\n",
                rcx, vr[0].w, vr[1].w);
    assert(rcx < center_x - 0.30f);
    assert(vr[1].w > vr[0].w); // right edge farther from camera when looking right

    // 3. Outside cab (10m behind) -> in_cab must be false
    CameraMemData cam_out = MakeCamera(0.0f, -10.0f, eye, true);
    assert(tracker.Update(cam_out, 512.0, 1400, in_cab));
    assert(!in_cab);
    std::printf("[PASS] Outside cab view detected -> auto hidden\n");
}

static void TestCoachAndMultiVehicle() {
    HudConfig cfg;
    CabAnchorTracker tracker;

    // 1. Start with standard truck eye
    Vec3f truck_eye{-0.45f, 1.55f, -0.80f};
    CameraMemData cam_truck = MakeCamera(0.0f, -7.0f, truck_eye, false);
    bool in_cab = false;
    assert(tracker.Update(cam_truck, 512.0, 1000, in_cab));
    assert(in_cab && tracker.HasAnchor());

    // 2. Teleport > 50m to a 14m Coach (e.g. Scania Touring / MAN Lion's Coach, eye around z=-5.90, y=-0.30)
    // WITHOUT calling Reset() manually - should auto-recalibrate via teleport detection!
    Vec3f coach_eye{-0.75f, -0.30f, -5.90f};
    CameraMemData cam_coach = MakeCamera(0.0f, -6.0f, coach_eye, false);
    cam_coach.truck_pos_x += 3000.0f;
    cam_coach.pos_x += 3000.0f;
    assert(tracker.Update(cam_coach, 512.0, 1500, in_cab));
    assert(in_cab && tracker.HasAnchor());
    assert(std::fabs(tracker.Anchor().z - coach_eye.z) < 0.01f);

    HudVertex vc[4]{};
    assert(ProjectHudQuad(cam_coach, tracker.Anchor(), cfg, 512.0, 1920.0f, 1080.0f, 0.94f, vc));
    float cx = 0.5f * ((vc[0].x / vc[0].w + 1.0f) * 0.5f + (vc[1].x / vc[1].w + 1.0f) * 0.5f);
    float cy = 0.5f * ((1.0f - vc[0].y / vc[0].w) * 0.5f + (1.0f - vc[2].y / vc[2].w) * 0.5f);
    std::printf("[PASS] 14m Coach auto-relock & projection: center=(%.3f, %.3f)\n", cx, cy);
    assert(cx > 0.50f && cx < 0.65f && cy > 0.45f && cy < 0.65f);

    // Coach outside chase view & roof view must hide
    CameraMemData coach_chase = MakeCamera(0.0f, -10.0f, {0.0f, 4.5f, 12.0f}, false);
    coach_chase.truck_pos_x = cam_coach.truck_pos_x;
    coach_chase.pos_x += 3000.0f;
    assert(tracker.Update(coach_chase, 512.0, 1600, in_cab));
    assert(!in_cab);

    CameraMemData coach_roof = MakeCamera(0.0f, -5.0f, {0.0f, 2.5f, -5.5f}, false);
    coach_roof.truck_pos_x = cam_coach.truck_pos_x;
    coach_roof.pos_x += 3000.0f;
    assert(tracker.Update(coach_roof, 512.0, 1700, in_cab));
    assert(!in_cab);

    // 3. Double-decker bus lower deck (eye around y=-1.25, z=-6.50) after explicit Reset()
    tracker.Reset();
    Vec3f dd_eye{-0.80f, -1.25f, -6.50f};
    CameraMemData cam_dd = MakeCamera(-25.0f, -5.0f, dd_eye, false);
    assert(tracker.Update(cam_dd, 512.0, 2000, in_cab));
    assert(in_cab && tracker.HasAnchor());
    assert(ProjectHudQuad(cam_dd, tracker.Anchor(), cfg, 512.0, 1920.0f, 1080.0f, 0.94f, vc));
    std::printf("[PASS] Double-decker coach lower deck (-0.80,-1.25,-6.50) & outside views verified\n");

    // 4. Per-vehicle config override isolation
    std::wstring tmp_ini = L"tests\\_tmp_vehicle_test.ini";
    DeleteFileW(tmp_ini.c_str());
    PluginConfig loaded;
    LoadConfig(tmp_ini, loaded);
    assert(std::fabs(loaded.hud.offset_x - 0.160f) < 1e-4f);

    std::string coach_id = SanitizeVehicleId("scania.touring/23");
    assert(coach_id == "scania.touring_23");
    assert(SaveConfigValue(tmp_ini, "Vehicle." + coach_id, "offset_x", "0.220"));
    assert(SaveConfigValue(tmp_ini, "Vehicle." + coach_id, "offset_z", "-1.100"));

    HudConfig coach_hud = loaded.hud;
    assert(LoadVehicleHudOverrides(tmp_ini, coach_id, coach_hud));
    assert(std::fabs(coach_hud.offset_x - 0.220f) < 1e-4f);
    assert(std::fabs(coach_hud.offset_y - (-0.245f)) < 1e-4f); // inherited from [HUD]
    assert(std::fabs(coach_hud.offset_z - (-1.100f)) < 1e-4f);

    HudConfig other_hud = loaded.hud;
    assert(!LoadVehicleHudOverrides(tmp_ini, "mercedes.actros2014", other_hud));
    assert(std::fabs(other_hud.offset_x - 0.160f) < 1e-4f);
    DeleteFileW(tmp_ini.c_str());
    std::printf("[PASS] Per-vehicle INI section [Vehicle.%s] isolated override verified\n", coach_id.c_str());
}

static void RenderSamples(const std::wstring& out_dir) {
    assert(HudPainter::InitGdiPlus());
    HudPainter painter;

    struct Case {
        const wchar_t* file;
        HudSnapshot snap;
        uint64_t t_ms;
    };
    Case cases[] = {
        {L"hud_v2_red.png", {true, Phase::Red, false, 14.3f, 128.0f, AdviceLevel::Adjust, "减速至 31 km/h 绿波通过", "", false}, 1000},
        {L"hud_v2_green_stop.png", {true, Phase::Green, false, 3.4f, 86.0f, AdviceLevel::Stop, "绿灯即将结束，准备减速停车", "", false}, 1000},
        {L"hud_v2_green_go.png", {true, Phase::Green, false, 18.0f, 92.0f, AdviceLevel::Go, "保持车速，可顺利通过", "", false}, 1000},
        {L"hud_v2_yellow.png", {true, Phase::Yellow, false, 1.8f, 22.0f, AdviceLevel::Warn, "黄灯，保持车速通过", "", false}, 1000},
        {L"hud_v2_flashing.png", {true, Phase::Flashing, false, 0.0f, 45.0f, AdviceLevel::Warn, "黄灯闪烁，减速观察让行", "3D X:+0.160 Y:-0.245 Z:-1.390", true}, 1000},
    };

    for (const auto& c : cases) {
        painter.Update(c.snap, 0.28f, c.t_ms);
        std::wstring path = out_dir + L"\\" + c.file;
        assert(painter.SavePng(path));
    }
    HudPainter::ShutdownGdiPlus();
    std::printf("[PASS] Rendered 5 HUD PNG samples to scratch dir\n");
}

int main() {
    TestStructs();
    TestLogic();
    Test3DProjection();
    TestCoachAndMultiVehicle();
    RenderSamples(L"C:\\Users\\CHRRY\\.gemini\\antigravity\\brain\\b40499df-1577-4224-a616-714bdd695429\\scratch");
    std::printf("ALL NATIVE TESTS PASSED\n");
    return 0;
}
