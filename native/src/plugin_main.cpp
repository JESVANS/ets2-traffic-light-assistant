#include <windows.h>

#include "scssdk_telemetry.h"
#include "eurotrucks2/scssdk_eut2.h"
#include "eurotrucks2/scssdk_telemetry_eut2.h"
#include "common/scssdk_telemetry_common_configs.h"
#include "common/scssdk_telemetry_truck_common_channels.h"

#include "config.hpp"
#include "d3d11_hook.hpp"
#include "hud_math.hpp"
#include "hud_painter.hpp"
#include "logic.hpp"
#include "shm_reader.hpp"
#include "types.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace tlhud {

namespace {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kCanvasW = HudPainter::kWidth;
constexpr int kCanvasH = HudPainter::kHeight;

struct KeyRepeater {
    int vk = 0;
    bool down_prev = false;
    uint64_t next_fire_ms = 0;

    bool Poll(uint64_t now_ms, bool repeatable) {
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!down) {
            down_prev = false;
            return false;
        }
        if (!down_prev) {
            down_prev = true;
            next_fire_ms = now_ms + 350;
            return true;
        }
        if (repeatable && now_ms >= next_fire_ms) {
            next_fire_ms = now_ms + 70;
            return true;
        }
        return false;
    }
};

struct PluginState {
    CRITICAL_SECTION cs{};
    bool cs_init = false;
    scs_log_t scs_log = nullptr;
    std::wstring ini_path;

    PluginConfig cfg;
    HudConfig base_hud_cfg;
    TruckTelemetry truck;

    // Protected by cs (written by SCS OnConfiguration, consumed by RenderCallback)
    std::string pending_vehicle_id;
    std::string pending_vehicle_brand;
    bool vehicle_changed = false;

    // Render-thread state
    std::string current_vehicle_id;
    bool prev_log_anchor = false;
    bool prev_log_in_cab = false;
    bool has_log_in_cab = false;

    ShmReader cam_shm{L"Local\\ETS2LACameraProps"};
    ShmReader sem_shm{L"Local\\ETS2LASemaphore"};
    CabAnchorTracker cab_tracker;
    TargetSelector selector;

    bool demo_mode = false;
    bool hud_hidden = false;
    std::string toast_text;
    uint64_t toast_until_ms = 0;

    // Sound alert debounce
    bool has_last_alert = false;
    int32_t last_alert_id = 0;
    AdviceLevel last_alert_level = AdviceLevel::Info;
    uint64_t last_alert_ms = 0;

    // Hotkey state (used on render thread)
    KeyRepeater k_left{VK_LEFT};
    KeyRepeater k_right{VK_RIGHT};
    KeyRepeater k_up{VK_UP};
    KeyRepeater k_down{VK_DOWN};
    KeyRepeater k_pgup{VK_PRIOR};
    KeyRepeater k_pgdn{VK_NEXT};
    KeyRepeater k_f8{VK_F8};
    KeyRepeater k_f9{VK_F9};
    KeyRepeater k_f10{VK_F10};
    KeyRepeater k_f11{VK_F11};
    bool hotkeys_ready = false;
};

PluginState g_state;

void LogMsg(scs_log_type_t level, const char* msg) {
    if (g_state.scs_log) {
        g_state.scs_log(level, msg);
    }
}

std::wstring GetSiblingIniPath(const void* addr_in_dll) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr_in_dll),
            &mod) || !mod) {
        return L"ets2_tl_hud.ini";
    }
    wchar_t path[MAX_PATH * 2]{};
    DWORD len = GetModuleFileNameW(mod, path, MAX_PATH * 2);
    if (len == 0) return L"ets2_tl_hud.ini";
    std::wstring s(path, len);
    size_t slash = s.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        s = s.substr(0, slash + 1);
    } else {
        s.clear();
    }
    return s + L"ets2_tl_hud.ini";
}

bool IsGameForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

std::string FormatFloat3(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", (double)v);
    return buf;
}

void SetToast(const std::string& text, uint64_t now_ms) {
    g_state.toast_text = text;
    g_state.toast_until_ms = now_ms + 2500;
}

void ProcessHotkeys(uint64_t now_ms) {
    if (!IsGameForeground()) {
        g_state.hotkeys_ready = false;
        return;
    }

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (!ctrl) {
        // Reset repeaters when Ctrl released so next press triggers cleanly
        g_state.k_left.down_prev = false;
        g_state.k_right.down_prev = false;
        g_state.k_up.down_prev = false;
        g_state.k_down.down_prev = false;
        g_state.k_pgup.down_prev = false;
        g_state.k_pgdn.down_prev = false;
        g_state.k_f8.down_prev = false;
        g_state.k_f9.down_prev = false;
        g_state.k_f10.down_prev = false;
        g_state.k_f11.down_prev = false;
        g_state.hotkeys_ready = true;
        return;
    }

    if (!g_state.hotkeys_ready) return;

    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool is_3d = (g_state.cfg.hud.mode != "screen_2d");

    if (g_state.k_f8.Poll(now_ms, false)) {
        g_state.cfg.hud.mode = is_3d ? "screen_2d" : "window_3d";
        SaveConfigValue(g_state.ini_path, "HUD", "mode", g_state.cfg.hud.mode);
        SetToast(is_3d ? "模式: 2D固定" : "模式: 3D车窗", now_ms);
        return;
    }

    if (g_state.k_f9.Poll(now_ms, false)) {
        g_state.demo_mode = !g_state.demo_mode;
        SetToast(g_state.demo_mode ? "演示模式: 开" : "演示模式: 关", now_ms);
        return;
    }

    if (g_state.k_f10.Poll(now_ms, false)) {
        LoadConfig(g_state.ini_path, g_state.cfg);
        g_state.base_hud_cfg = g_state.cfg.hud;
        LoadVehicleHudOverrides(g_state.ini_path, g_state.current_vehicle_id, g_state.cfg.hud);
        g_state.cab_tracker.Reset();
        g_state.prev_log_anchor = false;
        g_state.has_log_in_cab = false;
        SetToast("已重载配置并校准视角", now_ms);
        return;
    }

    if (g_state.k_f11.Poll(now_ms, false)) {
        g_state.hud_hidden = !g_state.hud_hidden;
        SetToast(g_state.hud_hidden ? "HUD 已隐藏" : "HUD 已显示", now_ms);
        return;
    }

    std::string pos_sec = g_state.current_vehicle_id.empty()
                          ? std::string("HUD")
                          : ("Vehicle." + g_state.current_vehicle_id);

    bool moved = false;
    if (is_3d) {
        float step_xy = shift ? 0.002f : 0.010f;
        float step_z = shift ? 0.005f : 0.020f;
        if (g_state.k_left.Poll(now_ms, true))  { g_state.cfg.hud.offset_x -= step_xy; moved = true; }
        if (g_state.k_right.Poll(now_ms, true)) { g_state.cfg.hud.offset_x += step_xy; moved = true; }
        if (g_state.k_up.Poll(now_ms, true))    { g_state.cfg.hud.offset_y += step_xy; moved = true; }
        if (g_state.k_down.Poll(now_ms, true))  { g_state.cfg.hud.offset_y -= step_xy; moved = true; }
        if (g_state.k_pgup.Poll(now_ms, true))  { g_state.cfg.hud.offset_z -= step_z; moved = true; }
        if (g_state.k_pgdn.Poll(now_ms, true))  { g_state.cfg.hud.offset_z += step_z; moved = true; }
        if (moved) {
            SaveConfigValue(g_state.ini_path, pos_sec, "offset_x", FormatFloat3(g_state.cfg.hud.offset_x));
            SaveConfigValue(g_state.ini_path, pos_sec, "offset_y", FormatFloat3(g_state.cfg.hud.offset_y));
            SaveConfigValue(g_state.ini_path, pos_sec, "offset_z", FormatFloat3(g_state.cfg.hud.offset_z));
            if (pos_sec == "HUD") {
                g_state.base_hud_cfg.offset_x = g_state.cfg.hud.offset_x;
                g_state.base_hud_cfg.offset_y = g_state.cfg.hud.offset_y;
                g_state.base_hud_cfg.offset_z = g_state.cfg.hud.offset_z;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "3D X:%+.3f Y:%+.3f Z:%+.3f",
                          (double)g_state.cfg.hud.offset_x,
                          (double)g_state.cfg.hud.offset_y,
                          (double)g_state.cfg.hud.offset_z);
            SetToast(buf, now_ms);
        }
    } else {
        float step_xy = shift ? 0.001f : 0.005f;
        float step_w = shift ? 0.002f : 0.010f;
        if (g_state.k_left.Poll(now_ms, true))  { g_state.cfg.hud.screen_x -= step_xy; moved = true; }
        if (g_state.k_right.Poll(now_ms, true)) { g_state.cfg.hud.screen_x += step_xy; moved = true; }
        if (g_state.k_up.Poll(now_ms, true))    { g_state.cfg.hud.screen_y -= step_xy; moved = true; }
        if (g_state.k_down.Poll(now_ms, true))  { g_state.cfg.hud.screen_y += step_xy; moved = true; }
        if (g_state.k_pgup.Poll(now_ms, true))  { g_state.cfg.hud.screen_width += step_w; moved = true; }
        if (g_state.k_pgdn.Poll(now_ms, true))  { g_state.cfg.hud.screen_width = std::max(0.08f, g_state.cfg.hud.screen_width - step_w); moved = true; }
        if (moved) {
            SaveConfigValue(g_state.ini_path, pos_sec, "screen_x", FormatFloat3(g_state.cfg.hud.screen_x));
            SaveConfigValue(g_state.ini_path, pos_sec, "screen_y", FormatFloat3(g_state.cfg.hud.screen_y));
            SaveConfigValue(g_state.ini_path, pos_sec, "screen_width", FormatFloat3(g_state.cfg.hud.screen_width));
            if (pos_sec == "HUD") {
                g_state.base_hud_cfg.screen_x = g_state.cfg.hud.screen_x;
                g_state.base_hud_cfg.screen_y = g_state.cfg.hud.screen_y;
                g_state.base_hud_cfg.screen_width = g_state.cfg.hud.screen_width;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "2D X:%.3f Y:%.3f W:%.3f",
                          (double)g_state.cfg.hud.screen_x,
                          (double)g_state.cfg.hud.screen_y,
                          (double)g_state.cfg.hud.screen_width);
            SetToast(buf, now_ms);
        }
    }
}

struct BeepArgs {
    int count;
    DWORD freq;
    DWORD dur;
};

DWORD WINAPI BeepThreadProc(LPVOID param) {
    BeepArgs* args = reinterpret_cast<BeepArgs*>(param);
    if (args) {
        for (int i = 0; i < args->count; ++i) {
            Beep(args->freq, args->dur);
            if (i + 1 < args->count) Sleep(50);
        }
        delete args;
    }
    return 0;
}

void MaybeSoundAlert(const TargetResult& target, const AdviceResult& advice, uint64_t now_ms) {
    if (!g_state.cfg.hud.sound_alert) return;

    Phase p = target.sem.phase;
    bool dangerous = (p == Phase::Red || p == Phase::Yellow ||
                      p == Phase::GateClosed || p == Phase::GateClosing);
    if (!dangerous) return;

    BeepArgs args{};
    if (advice.level == AdviceLevel::Warn) {
        args = {3, 1400, 120};
    } else if (advice.level == AdviceLevel::Stop && target.distance < 150.0f) {
        args = {1, 900, 180};
    } else {
        return;
    }

    if (g_state.has_last_alert &&
        ((g_state.last_alert_id == target.sem.id && g_state.last_alert_level == advice.level) ||
         (now_ms - g_state.last_alert_ms < 4000))) {
        return;
    }

    g_state.has_last_alert = true;
    g_state.last_alert_id = target.sem.id;
    g_state.last_alert_level = advice.level;
    g_state.last_alert_ms = now_ms;

    auto* heap_args = new BeepArgs(args);
    HANDLE th = CreateThread(nullptr, 0, BeepThreadProc, heap_args, 0, nullptr);
    if (th) CloseHandle(th);
    else delete heap_args;
}

bool RenderCallback(float vp_width,
                    float vp_height,
                    HudVertex out_verts[4],
                    HudPainter& painter,
                    void*) {
    uint64_t now_ms = GetTickCount64();
    ProcessHotkeys(now_ms);

    if (g_state.hud_hidden) return false;

    TruckTelemetry truck_copy;
    bool switched = false;
    std::string new_vid;
    std::string new_brand;
    EnterCriticalSection(&g_state.cs);
    truck_copy = g_state.truck;
    if (g_state.vehicle_changed) {
        switched = true;
        new_vid = g_state.pending_vehicle_id;
        new_brand = g_state.pending_vehicle_brand;
        g_state.vehicle_changed = false;
    }
    LeaveCriticalSection(&g_state.cs);

    if (switched) {
        g_state.current_vehicle_id = new_vid;
        g_state.cab_tracker.Reset();
        g_state.selector.Reset();
        g_state.cfg.hud = g_state.base_hud_cfg;
        LoadVehicleHudOverrides(g_state.ini_path, g_state.current_vehicle_id, g_state.cfg.hud);
        g_state.prev_log_anchor = false;
        g_state.has_log_in_cab = false;
        char log_buf[192];
        std::snprintf(log_buf, sizeof(log_buf), "[ets2_tl_hud] Vehicle switched: id=%s brand=%s",
                      new_vid.empty() ? "<none>" : new_vid.c_str(),
                      new_brand.empty() ? "<none>" : new_brand.c_str());
        LogMsg(SCS_LOG_TYPE_message, log_buf);
    }

    if (truck_copy.paused) return false;

    CameraMemData cam{};
    bool has_cam = g_state.cam_shm.EnsureOpen(now_ms) && g_state.cam_shm.ReadStruct(cam);
    bool in_cab = true;
    bool anchor_ready = false;

    if (has_cam) {
        bool valid_cam = g_state.cab_tracker.Update(cam, (double)g_state.cfg.source.sector_size, now_ms, in_cab);
        if (valid_cam) {
            anchor_ready = g_state.cab_tracker.HasAnchor();
            if (!g_state.prev_log_anchor && anchor_ready) {
                g_state.prev_log_anchor = true;
                Vec3f a = g_state.cab_tracker.Anchor();
                char log_buf[192];
                std::snprintf(log_buf, sizeof(log_buf),
                              "[ets2_tl_hud] Cab anchor locked: vehicle=%s anchor=(%.2f, %.2f, %.2f)",
                              g_state.current_vehicle_id.empty() ? "<default>" : g_state.current_vehicle_id.c_str(),
                              (double)a.x, (double)a.y, (double)a.z);
                LogMsg(SCS_LOG_TYPE_message, log_buf);
            }
            if (anchor_ready && (!g_state.has_log_in_cab || g_state.prev_log_in_cab != in_cab)) {
                g_state.has_log_in_cab = true;
                g_state.prev_log_in_cab = in_cab;
                Vec3f loc = g_state.cab_tracker.LastCamLocal();
                char log_buf[160];
                std::snprintf(log_buf, sizeof(log_buf),
                              "[ets2_tl_hud] Cab view state: in_cab=%d local=(%.2f, %.2f, %.2f)",
                              in_cab ? 1 : 0, (double)loc.x, (double)loc.y, (double)loc.z);
                LogMsg(SCS_LOG_TYPE_message, log_buf);
            }
            if (g_state.cfg.hud.hide_outside_cab && !in_cab) {
                return false;
            }
        } else {
            has_cam = false;
        }
    }

    HudSnapshot snap;
    if (g_state.demo_mode) {
        snap = BuildDemoSnapshot(now_ms, g_state.cfg.advisor);
    } else {
        if (!truck_copy.on_world) return false;
        if (truck_copy.last_update_ms > 0 &&
            (now_ms - truck_copy.last_update_ms) > (uint64_t)(g_state.cfg.source.stale_timeout * 1000.0f)) {
            return false;
        }

        SemaphoreMemData sem_mem{};
        if (!g_state.sem_shm.EnsureOpen(now_ms) || !g_state.sem_shm.ReadStruct(sem_mem)) {
            return false;
        }

        auto sems = ParseSemaphores(sem_mem, g_state.cfg.source, g_state.cfg.states);
        TargetResult target;
        if (!g_state.selector.Select(truck_copy, sems, g_state.cfg.selection, target)) {
            return false;
        }

        AdviceResult advice = ComputeAdvice(target, truck_copy, g_state.cfg.advisor);
        MaybeSoundAlert(target, advice, now_ms);

        snap.visible = true;
        snap.phase = target.sem.phase;
        snap.is_gate = (target.sem.type == TYPE_GATE);
        snap.time_left = target.sem.time_left;
        snap.distance = target.distance;
        snap.advice_level = advice.level;
        snap.advice_text = advice.text;
        snap.is_demo = false;
    }

    if (now_ms < g_state.toast_until_ms && !g_state.toast_text.empty()) {
        snap.toast_text = g_state.toast_text;
    } else {
        g_state.toast_text.clear();
    }

    float alpha = g_state.cfg.hud.hud_alpha;
    bool use_3d = (g_state.cfg.hud.mode != "screen_2d") && has_cam && anchor_ready;
    if (use_3d) {
        if (!ProjectHudQuad(cam,
                            g_state.cab_tracker.Anchor(),
                            g_state.cfg.hud,
                            (double)g_state.cfg.source.sector_size,
                            vp_width,
                            vp_height,
                            alpha,
                            out_verts)) {
            return false;
        }
    } else {
        BuildScreen2DQuad(g_state.cfg.hud,
                          vp_width,
                          vp_height,
                          (float)kCanvasH / (float)kCanvasW,
                          alpha,
                          out_verts);
    }

    painter.Update(snap, g_state.cfg.hud.bg_opacity, now_ms);
    return true;
}

// ---------- SCS Telemetry Callbacks ----------

SCSAPI_VOID OnPaused(const scs_event_t, const void* const, scs_context_t) {
    if (!g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.paused = true;
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnStarted(const scs_event_t, const void* const, scs_context_t) {
    if (!g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.paused = false;
    g_state.truck.last_update_ms = GetTickCount64();
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnFrameEnd(const scs_event_t, const void* const, scs_context_t) {
    if (!g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.last_update_ms = GetTickCount64();
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnConfiguration(const scs_event_t, const void* const event_info, scs_context_t) {
    if (!event_info || !g_state.cs_init) return;
    const auto* info = static_cast<const scs_telemetry_configuration_t*>(event_info);
    if (!info->id || std::strcmp(info->id, SCS_TELEMETRY_CONFIG_truck) != 0) return;

    std::string vid;
    std::string brand;
    if (info->attributes) {
        for (const scs_named_value_t* a = info->attributes; a->name != nullptr; ++a) {
            if (a->value.type != SCS_VALUE_TYPE_string || !a->value.value_string.value) continue;
            if (std::strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_id) == 0) {
                vid = SanitizeVehicleId(a->value.value_string.value);
            } else if (std::strcmp(a->name, SCS_TELEMETRY_CONFIG_ATTRIBUTE_brand) == 0) {
                brand = a->value.value_string.value;
            }
        }
    }

    EnterCriticalSection(&g_state.cs);
    g_state.pending_vehicle_id = vid;
    g_state.pending_vehicle_brand = brand;
    g_state.vehicle_changed = true;
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnWorldPlacement(const scs_string_t, const scs_u32_t, const scs_value_t* const value, scs_context_t) {
    if (!value || value->type != SCS_VALUE_TYPE_dplacement || !g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.on_world = true;
    g_state.truck.x = value->value_dplacement.position.x;
    g_state.truck.y = value->value_dplacement.position.y;
    g_state.truck.z = value->value_dplacement.position.z;
    g_state.truck.heading = (double)value->value_dplacement.orientation.heading * kTwoPi;
    g_state.truck.last_update_ms = GetTickCount64();
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnSpeed(const scs_string_t, const scs_u32_t, const scs_value_t* const value, scs_context_t) {
    if (!value || value->type != SCS_VALUE_TYPE_float || !g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.speed = value->value_float.value;
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnSpeedLimit(const scs_string_t, const scs_u32_t, const scs_value_t* const value, scs_context_t) {
    if (!value || value->type != SCS_VALUE_TYPE_float || !g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.speed_limit = value->value_float.value;
    LeaveCriticalSection(&g_state.cs);
}

SCSAPI_VOID OnCruiseSpeed(const scs_string_t, const scs_u32_t, const scs_value_t* const value, scs_context_t) {
    if (!value || value->type != SCS_VALUE_TYPE_float || !g_state.cs_init) return;
    EnterCriticalSection(&g_state.cs);
    g_state.truck.cruise_speed = value->value_float.value;
    LeaveCriticalSection(&g_state.cs);
}

} // namespace
} // namespace tlhud

extern "C" {

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t* const params) {
    if (version != SCS_TELEMETRY_VERSION_1_00 && version != SCS_TELEMETRY_VERSION_1_01) {
        return SCS_RESULT_unsupported;
    }
    if (!params) return SCS_RESULT_invalid_parameter;

    const auto* init = static_cast<const scs_telemetry_init_params_v100_t*>(params);

    if (!tlhud::g_state.cs_init) {
        InitializeCriticalSection(&tlhud::g_state.cs);
        tlhud::g_state.cs_init = true;
    }
    tlhud::g_state.scs_log = init->common.log;
    tlhud::g_state.ini_path = tlhud::GetSiblingIniPath(reinterpret_cast<const void*>(&scs_telemetry_init));

    tlhud::LoadConfig(tlhud::g_state.ini_path, tlhud::g_state.cfg);
    tlhud::g_state.base_hud_cfg = tlhud::g_state.cfg.hud;
    tlhud::g_state.demo_mode = tlhud::g_state.cfg.hud.start_demo;

    if (!tlhud::HudPainter::InitGdiPlus()) {
        tlhud::LogMsg(SCS_LOG_TYPE_error, "[ets2_tl_hud] Failed to initialize GDI+");
        return SCS_RESULT_generic_error;
    }

    init->register_for_event(SCS_TELEMETRY_EVENT_paused, tlhud::OnPaused, nullptr);
    init->register_for_event(SCS_TELEMETRY_EVENT_started, tlhud::OnStarted, nullptr);
    init->register_for_event(SCS_TELEMETRY_EVENT_frame_end, tlhud::OnFrameEnd, nullptr);
    init->register_for_event(SCS_TELEMETRY_EVENT_configuration, tlhud::OnConfiguration, nullptr);

    init->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_world_placement, SCS_U32_NIL,
                               SCS_VALUE_TYPE_dplacement, SCS_TELEMETRY_CHANNEL_FLAG_none,
                               tlhud::OnWorldPlacement, nullptr);
    init->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_speed, SCS_U32_NIL,
                               SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                               tlhud::OnSpeed, nullptr);
    init->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_navigation_speed_limit, SCS_U32_NIL,
                               SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                               tlhud::OnSpeedLimit, nullptr);
    init->register_for_channel(SCS_TELEMETRY_TRUCK_CHANNEL_cruise_control, SCS_U32_NIL,
                               SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none,
                               tlhud::OnCruiseSpeed, nullptr);

    tlhud::StartD3D11Hook(tlhud::RenderCallback, nullptr);
    tlhud::LogMsg(SCS_LOG_TYPE_message, "[ets2_tl_hud] Initialized 3D windshield HUD plugin");
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void) {
    tlhud::StopD3D11Hook();
    tlhud::g_state.cam_shm.Close();
    tlhud::g_state.sem_shm.Close();
    tlhud::HudPainter::ShutdownGdiPlus();
    if (tlhud::g_state.cs_init) {
        DeleteCriticalSection(&tlhud::g_state.cs);
        tlhud::g_state.cs_init = false;
    }
    tlhud::LogMsg(SCS_LOG_TYPE_message, "[ets2_tl_hud] Shutdown complete");
}

} // extern "C"
