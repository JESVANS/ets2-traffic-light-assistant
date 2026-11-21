#include "config.hpp"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <vector>

namespace tlhud {

namespace {

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && (unsigned char)s[start] <= ' ') start++;
    size_t end = s.size();
    while (end > start && (unsigned char)s[end - 1] <= ' ') end--;
    return s.substr(start, end - start);
}

std::string StripInlineComment(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ';' || s[i] == '#') {
            return Trim(s.substr(0, i));
        }
    }
    return Trim(s);
}

std::string ToLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool ParseBool(const std::string& v, bool fallback) {
    std::string lo = ToLower(Trim(v));
    if (lo == "1" || lo == "true" || lo == "yes" || lo == "on") return true;
    if (lo == "0" || lo == "false" || lo == "no" || lo == "off") return false;
    return fallback;
}

float ParseFloat(const std::string& v, float fallback) {
    char* end = nullptr;
    float r = std::strtof(v.c_str(), &end);
    return (end != v.c_str()) ? r : fallback;
}

int ParseInt(const std::string& v, int fallback) {
    char* end = nullptr;
    long r = std::strtol(v.c_str(), &end, 10);
    if (end == v.c_str()) return fallback;
    return (int)r;
}

bool ReadFileUtf8(const std::wstring& path, std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    content.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    bool ok = (sz.QuadPart == 0) || ReadFile(h, &content[0], (DWORD)sz.QuadPart, &rd, nullptr);
    CloseHandle(h);
    if (ok && content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF &&
        (unsigned char)content[1] == 0xBB &&
        (unsigned char)content[2] == 0xBF) {
        content.erase(0, 3);
    }
    return ok;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    bool ok = WriteFile(h, content.data(), (DWORD)content.size(), &wr, nullptr) && wr == content.size();
    CloseHandle(h);
    return ok;
}

void ApplyHudOption(HudConfig& h, const std::string& key, const std::string& val, bool allow_global) {
    if (key == "offset_x") h.offset_x = ParseFloat(val, h.offset_x);
    else if (key == "offset_y") h.offset_y = ParseFloat(val, h.offset_y);
    else if (key == "offset_z") h.offset_z = ParseFloat(val, h.offset_z);
    else if (key == "panel_width") h.panel_width = std::max(0.10f, ParseFloat(val, h.panel_width));
    else if (key == "panel_height") h.panel_height = std::max(0.05f, ParseFloat(val, h.panel_height));
    else if (key == "tilt_deg") h.tilt_deg = ParseFloat(val, h.tilt_deg);
    else if (key == "screen_x") h.screen_x = ParseFloat(val, h.screen_x);
    else if (key == "screen_y") h.screen_y = ParseFloat(val, h.screen_y);
    else if (key == "screen_width") h.screen_width = std::max(0.05f, ParseFloat(val, h.screen_width));
    else if (allow_global) {
        if (key == "mode") h.mode = ToLower(val);
        else if (key == "bg_opacity") h.bg_opacity = std::clamp(ParseFloat(val, h.bg_opacity), 0.0f, 1.0f);
        else if (key == "hud_alpha") h.hud_alpha = std::clamp(ParseFloat(val, h.hud_alpha), 0.05f, 1.0f);
        else if (key == "hide_outside_cab") h.hide_outside_cab = ParseBool(val, h.hide_outside_cab);
        else if (key == "sound_alert") h.sound_alert = ParseBool(val, h.sound_alert);
        else if (key == "start_demo") h.start_demo = ParseBool(val, h.start_demo);
    }
}

void ApplyOption(PluginConfig& cfg, const std::string& section, const std::string& key, const std::string& val) {
    if (section == "HUD") {
        ApplyHudOption(cfg.hud, key, val, true);
    } else if (section == "Source") {
        if (key == "sector_size") cfg.source.sector_size = ParseFloat(val, cfg.source.sector_size);
        else if (key == "stale_timeout") cfg.source.stale_timeout = ParseFloat(val, cfg.source.stale_timeout);
    } else if (section == "Selection") {
        if (key == "max_distance") cfg.selection.max_distance = ParseFloat(val, cfg.selection.max_distance);
        else if (key == "max_lateral") cfg.selection.max_lateral = ParseFloat(val, cfg.selection.max_lateral);
        else if (key == "lateral_growth") cfg.selection.lateral_growth = ParseFloat(val, cfg.selection.lateral_growth);
        else if (key == "max_height_diff") cfg.selection.max_height_diff = ParseFloat(val, cfg.selection.max_height_diff);
        else if (key == "parallel_threshold") cfg.selection.parallel_threshold = ParseFloat(val, cfg.selection.parallel_threshold);
        else if (key == "facing_mode") cfg.selection.facing_mode = ParseInt(val, cfg.selection.facing_mode);
        else if (key == "stop_line_offset") cfg.selection.stop_line_offset = ParseFloat(val, cfg.selection.stop_line_offset);
        else if (key == "lateral_weight") cfg.selection.lateral_weight = ParseFloat(val, cfg.selection.lateral_weight);
        else if (key == "include_gates") cfg.selection.include_gates = ParseBool(val, cfg.selection.include_gates);
    } else if (section == "Advisor") {
        if (key == "comfort_decel") cfg.advisor.comfort_decel = ParseFloat(val, cfg.advisor.comfort_decel);
        else if (key == "max_decel") cfg.advisor.max_decel = ParseFloat(val, cfg.advisor.max_decel);
        else if (key == "max_accel") cfg.advisor.max_accel = ParseFloat(val, cfg.advisor.max_accel);
        else if (key == "min_glide_speed_kmh") cfg.advisor.min_glide_speed_kmh = ParseFloat(val, cfg.advisor.min_glide_speed_kmh);
        else if (key == "default_max_speed_kmh") cfg.advisor.default_max_speed_kmh = ParseFloat(val, cfg.advisor.default_max_speed_kmh);
        else if (key == "red_yellow_duration") cfg.advisor.red_yellow_duration = ParseFloat(val, cfg.advisor.red_yellow_duration);
        else if (key == "use_speed_limit") cfg.advisor.use_speed_limit = ParseBool(val, cfg.advisor.use_speed_limit);
    } else if (section == "StateCodes") {
        if (key == "off") cfg.states.off = ParseInt(val, cfg.states.off);
        else if (key == "yellow") cfg.states.yellow = ParseInt(val, cfg.states.yellow);
        else if (key == "red") cfg.states.red = ParseInt(val, cfg.states.red);
        else if (key == "red_yellow") cfg.states.red_yellow = ParseInt(val, cfg.states.red_yellow);
        else if (key == "green") cfg.states.green = ParseInt(val, cfg.states.green);
        else if (key == "flashing") cfg.states.flashing = ParseInt(val, cfg.states.flashing);
        else if (key == "gate_closing") cfg.states.gate_closing = ParseInt(val, cfg.states.gate_closing);
        else if (key == "gate_closed") cfg.states.gate_closed = ParseInt(val, cfg.states.gate_closed);
        else if (key == "gate_opening") cfg.states.gate_opening = ParseInt(val, cfg.states.gate_opening);
        else if (key == "gate_open") cfg.states.gate_open = ParseInt(val, cfg.states.gate_open);
    }
}

} // namespace

const char* GetDefaultIniContent() {
    return
"; ============================================================\n"
";  ETS2 车窗红绿灯 HUD 插件配置文件 (ets2_tl_hud.ini)\n"
";  放置于 Euro Truck Simulator 2\\bin\\win_x64\\plugins\\ 目录\n"
";  游戏内按 Ctrl + F10 可随时热重载，无需重启游戏\n"
"; ============================================================\n\n"
"[HUD]\n"
"; 显示模式：\n"
";   window_3d = 3D 车窗投影（默认，固定在驾驶室前挡风玻璃上，转头随车窗透视移动，车外自动隐藏）\n"
";   screen_2d = 游戏内 2D 固定位置 HUD（始终固定在游戏画面绿框处）\n"
"mode = window_3d\n\n"
"; 3D 车窗模式相对驾驶员正视眼睛基准点的位置偏移（单位：米）\n"
";   offset_x：正数向右，负数向左（默认 +0.160 避开左侧导航 HUD，位于绿框处）\n"
";   offset_y：正数向上，负数向下（默认 -0.245 位于方向盘正上方、挡风玻璃下沿）\n"
";   offset_z：负数向前（默认 -1.390 贴合前挡风玻璃）\n"
"; 游戏内也可按 Ctrl + 方向键 / PageUp / PageDown 实时微调并自动保存！\n"
"offset_x = 0.160\n"
"offset_y = -0.245\n"
"offset_z = -1.390\n\n"
"; 3D 车窗 HUD 面板尺寸（米）与向驾驶员仰角（度）\n"
"panel_width = 0.420\n"
"panel_height = 0.155\n"
"tilt_deg = 8.0\n\n"
"; 2D 模式（或未检测到相机数据时自动退化）的屏幕归一化位置 [0.0 ~ 1.0]\n"
"screen_x = 0.560\n"
"screen_y = 0.535\n"
"screen_width = 0.210\n\n"
"; 玻璃投影底衬不透明度（0.0 = 无底框纯投影，0.28 = 通透微暗滤光玻璃）\n"
"bg_opacity = 0.28\n"
"; 整体 HUD 不透明度（0.0 ~ 1.0）\n"
"hud_alpha = 0.94\n\n"
"; 切到车外视角时自动隐藏车窗 HUD\n"
"hide_outside_cab = true\n"
"; 红/黄灯需要停车或紧急制动时蜂鸣提醒\n"
"sound_alert = true\n"
"; 启动时默认开启演示预览（方便首次调整位置，调完后改回 false，也可在游戏里按 Ctrl+F9 切换）\n"
"start_demo = false\n\n"
"[Source]\n"
"sector_size = 512.0\n"
"stale_timeout = 3.0\n\n"
"[Selection]\n"
"max_distance = 300.0\n"
"max_lateral = 12.0\n"
"lateral_growth = 0.12\n"
"max_height_diff = 12.0\n"
"parallel_threshold = 0.6\n"
"facing_mode = 0\n"
"stop_line_offset = 4.0\n"
"lateral_weight = 3.0\n"
"include_gates = true\n\n"
"[Advisor]\n"
"comfort_decel = 2.0\n"
"max_decel = 4.5\n"
"max_accel = 0.6\n"
"min_glide_speed_kmh = 20.0\n"
"default_max_speed_kmh = 90.0\n"
"red_yellow_duration = 2.0\n"
"use_speed_limit = true\n\n"
"[StateCodes]\n"
"off = 0\n"
"yellow = 1\n"
"red = 2\n"
"red_yellow = 4\n"
"green = 8\n"
"flashing = 32\n"
"gate_closing = 0\n"
"gate_closed = 1\n"
"gate_opening = 2\n"
"gate_open = 3\n";
}

bool LoadConfig(const std::wstring& path, PluginConfig& out) {
    out = PluginConfig();
    std::string content;
    if (!ReadFileUtf8(path, content)) {
        WriteFileUtf8(path, GetDefaultIniContent());
        return false;
    }

    std::istringstream iss(content);
    std::string line;
    std::string section;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = Trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ToLower(Trim(trimmed.substr(0, eq)));
        std::string val = StripInlineComment(trimmed.substr(eq + 1));
        ApplyOption(out, section, key, val);
    }
    return true;
}

std::string SanitizeVehicleId(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out.push_back((char)c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

bool LoadVehicleHudOverrides(const std::wstring& path,
                             const std::string& vehicle_id,
                             HudConfig& hud) {
    if (vehicle_id.empty()) return false;
    std::string content;
    if (!ReadFileUtf8(path, content)) return false;

    std::string target_sec = "Vehicle." + SanitizeVehicleId(vehicle_id);
    std::istringstream iss(content);
    std::string line;
    std::string section;
    bool found = false;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') continue;
        if (trimmed.front() == '[' && trimmed.back() == ']') {
            section = Trim(trimmed.substr(1, trimmed.size() - 2));
            continue;
        }
        if (section != target_sec) continue;
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = ToLower(Trim(trimmed.substr(0, eq)));
        std::string val = StripInlineComment(trimmed.substr(eq + 1));
        ApplyHudOption(hud, key, val, false);
        found = true;
    }
    return found;
}

bool SaveConfigValue(const std::wstring& path,
                     const std::string& section,
                     const std::string& key,
                     const std::string& value) {
    std::string content;
    if (!ReadFileUtf8(path, content)) {
        content = GetDefaultIniContent();
    }

    std::vector<std::string> lines;
    {
        std::istringstream iss(content);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
        }
    }

    std::string cur_sec;
    std::string target_key_lo = ToLower(key);
    bool updated = false;
    int sec_insert_idx = -1;

    for (size_t i = 0; i < lines.size(); ++i) {
        std::string tr = Trim(lines[i]);
        if (!tr.empty() && tr.front() == '[' && tr.back() == ']') {
            if (cur_sec == section && sec_insert_idx == -1) {
                sec_insert_idx = (int)i;
            }
            cur_sec = Trim(tr.substr(1, tr.size() - 2));
            continue;
        }
        if (cur_sec == section && !tr.empty() && tr[0] != ';' && tr[0] != '#') {
            size_t eq = tr.find('=');
            if (eq != std::string::npos) {
                std::string k = ToLower(Trim(tr.substr(0, eq)));
                if (k == target_key_lo) {
                    lines[i] = key + " = " + value;
                    updated = true;
                    break;
                }
            }
        }
    }

    if (!updated) {
        std::string new_line = key + " = " + value;
        if (sec_insert_idx >= 0) {
            lines.insert(lines.begin() + sec_insert_idx, new_line);
        } else if (cur_sec == section) {
            lines.push_back(new_line);
        } else {
            lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(new_line);
        }
    }

    std::string out_buf;
    for (size_t i = 0; i < lines.size(); ++i) {
        out_buf += lines[i];
        out_buf += '\n';
    }
    return WriteFileUtf8(path, out_buf);
}

} // namespace tlhud
