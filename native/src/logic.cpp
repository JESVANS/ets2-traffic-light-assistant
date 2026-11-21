#include "logic.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace tlhud {

namespace {

std::string FormatIntText(const char* prefix, int val, const char* suffix) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s%d%s", prefix, val, suffix);
    return std::string(buf);
}

void SemaphoreForwardXZ(const SemaphoreParsed& s, float& fx, float& fz) {
    float w = s.qw, x = s.qx, y = s.qy, z = s.qz;
    float vx = -2.0f * (x * z + w * y);
    float vz = -(1.0f - 2.0f * (x * x + y * y));
    float n = std::hypot(vx, vz);
    if (n > 1e-6f) {
        fx = vx / n;
        fz = vz / n;
    } else {
        fx = 0.0f;
        fz = 0.0f;
    }
}

} // namespace

Phase MapPhase(int32_t sem_type, int32_t state, const StateCodesConfig& codes) {
    if (sem_type == TYPE_GATE) {
        if (state == codes.gate_closed) return Phase::GateClosed;
        if (state == codes.gate_closing) return Phase::GateClosing;
        if (state == codes.gate_opening) return Phase::GateOpening;
        if (state == codes.gate_open) return Phase::GateOpen;
        return Phase::Unknown;
    }
    if (state == codes.off) return Phase::Off;
    if (state == codes.red) return Phase::Red;
    if (state == codes.red_yellow) return Phase::RedYellow;
    if (state == codes.green) return Phase::Green;
    if (state == codes.yellow) return Phase::Yellow;
    if (state == codes.flashing) return Phase::Flashing;
    return Phase::Unknown;
}

std::vector<SemaphoreParsed> ParseSemaphores(const SemaphoreMemData& mem,
                                             const SourceConfig& src_cfg,
                                             const StateCodesConfig& codes) {
    std::vector<SemaphoreParsed> out;
    out.reserve(40);
    double sector = (double)src_cfg.sector_size;
    for (int i = 0; i < 40; ++i) {
        const SemaphoreObject& o = mem.items[i];
        if (o.type != TYPE_TRAFFIC_LIGHT && o.type != TYPE_GATE) continue;
        if (!std::isfinite(o.x) || !std::isfinite(o.y) || !std::isfinite(o.z) ||
            !std::isfinite(o.qw) || !std::isfinite(o.qx) || !std::isfinite(o.qy) ||
            !std::isfinite(o.qz) || !std::isfinite(o.time_remaining)) {
            continue;
        }
        SemaphoreParsed s;
        s.index = i;
        s.type = o.type;
        s.state = o.state;
        s.phase = MapPhase(o.type, o.state, codes);
        s.time_left = o.time_remaining;
        s.id = o.id;
        s.wx = (double)o.x + (double)o.cx * sector;
        s.wy = (double)o.y;
        s.wz = (double)o.z + (double)o.cz * sector;
        s.qw = o.qw;
        s.qx = o.qx;
        s.qy = o.qy;
        s.qz = o.qz;
        out.push_back(s);
    }
    return out;
}

void RelativeGeometry(const TruckTelemetry& truck,
                      const SemaphoreParsed& sem,
                      float& out_lon,
                      float& out_lat,
                      float& out_dy,
                      float& out_facing_dot) {
    float fx = -(float)std::sin(truck.heading);
    float fz = -(float)std::cos(truck.heading);
    float dx = (float)(sem.wx - truck.x);
    float dz = (float)(sem.wz - truck.z);
    out_lon = dx * fx + dz * fz;
    out_lat = dx * (-fz) + dz * fx;
    out_dy = (float)(sem.wy - truck.y);
    float lfx = 0.0f, lfz = 0.0f;
    SemaphoreForwardXZ(sem, lfx, lfz);
    out_facing_dot = lfx * fx + lfz * fz;
}

bool TargetSelector::Select(const TruckTelemetry& truck,
                            const std::vector<SemaphoreParsed>& sems,
                            const SelectionConfig& cfg,
                            TargetResult& out) {
    float best_score = std::numeric_limits<float>::infinity();
    bool found = false;

    for (const auto& s : sems) {
        if (s.type == TYPE_GATE && !cfg.include_gates) continue;
        if (s.phase == Phase::Off || s.phase == Phase::Unknown) continue;

        float lon = 0.0f, lat = 0.0f, dy = 0.0f, dot = 0.0f;
        RelativeGeometry(truck, s, lon, lat, dy, dot);

        if (lon < 1.0f || lon > cfg.max_distance) continue;
        if (std::fabs(lat) > cfg.max_lateral + lon * cfg.lateral_growth) continue;
        if (std::fabs(dy) > cfg.max_height_diff) continue;

        if (s.type == TYPE_TRAFFIC_LIGHT) {
            float thr = cfg.parallel_threshold;
            if (cfg.facing_mode == 0 && std::fabs(dot) < thr) continue;
            if (cfg.facing_mode == 1 && dot < thr) continue;
            if (cfg.facing_mode == -1 && dot > -thr) continue;
        }

        float score = lon + cfg.lateral_weight * std::fabs(lat);
        long rx = std::lround(s.wx);
        long rz = std::lround(s.wz);
        if (has_prev_ && s.id == prev_id_ && rx == prev_rx_ && rz == prev_rz_) {
            score *= 0.85f;
        }

        if (score < best_score) {
            best_score = score;
            out.sem = s;
            out.distance = std::max(0.0f, lon - cfg.stop_line_offset);
            out.longitudinal = lon;
            out.lateral = lat;
            found = true;
        }
    }

    if (found) {
        has_prev_ = true;
        prev_id_ = out.sem.id;
        prev_rx_ = std::lround(out.sem.wx);
        prev_rz_ = std::lround(out.sem.wz);
    } else {
        has_prev_ = false;
    }
    return found;
}

AdviceResult ComputeAdvice(const TargetResult& target,
                           const TruckTelemetry& truck,
                           const AdvisorConfig& cfg) {
    const SemaphoreParsed& sem = target.sem;
    float d = target.distance;
    float v = std::max(0.0f, truck.speed);
    float t = std::max(0.0f, sem.time_left);
    float vmax = (cfg.use_speed_limit && truck.speed_limit > 1.0f)
                 ? truck.speed_limit
                 : (cfg.default_max_speed_kmh / 3.6f);
    float eta = (v > 0.5f) ? (d / v) : std::numeric_limits<float>::infinity();
    float stop_decel = (d > 0.5f)
                       ? ((v * v) / (2.0f * d))
                       : ((v > 0.5f) ? std::numeric_limits<float>::infinity() : 0.0f);
    bool stopped = (v < 0.5f);

    if (sem.type == TYPE_GATE) {
        if (sem.phase == Phase::GateOpen) return {AdviceLevel::Go, "道闸已开启", 0.0f, false};
        if (stopped) return {AdviceLevel::Info, "等待道闸开启", 0.0f, false};
        int dm = (int)std::lround(d);
        return {AdviceLevel::Stop, FormatIntText("道闸未开，前方 ", dm, " m 停车"), 0.0f, false};
    }

    switch (sem.phase) {
    case Phase::Green: {
        if (stopped && d < 15.0f) {
            return {AdviceLevel::Go, "绿灯，起步通过", 0.0f, false};
        }
        if (eta <= t) {
            return {AdviceLevel::Go, "保持车速，可顺利通过", 0.0f, false};
        }
        if (t > 0.5f) {
            float v_need = d / t;
            float acc_need = (d > 0.0f) ? ((v_need * v_need - v * v) / (2.0f * d)) : std::numeric_limits<float>::infinity();
            if (v_need <= vmax && acc_need <= cfg.max_accel) {
                float kmh = v_need * 3.6f;
                return {AdviceLevel::Adjust, FormatIntText("加速至 ", (int)std::lround(kmh), " km/h 可通过"), kmh, true};
            }
        }
        if (stop_decel <= cfg.max_decel) {
            return {AdviceLevel::Stop, "绿灯即将结束，准备减速停车", 0.0f, false};
        }
        return {AdviceLevel::Warn, "无法安全停车，谨慎通过", 0.0f, false};
    }
    case Phase::Yellow: {
        if (eta <= t + 0.5f) {
            return {AdviceLevel::Warn, "黄灯，保持车速通过", 0.0f, false};
        }
        if (stop_decel <= cfg.max_decel) {
            int dm = (int)std::lround(d);
            return {AdviceLevel::Stop, FormatIntText("黄灯，减速停车（", dm, " m）"), 0.0f, false};
        }
        return {AdviceLevel::Warn, "黄灯，无法安全停车，谨慎通过", 0.0f, false};
    }
    case Phase::Red:
    case Phase::RedYellow: {
        float green_in = t + (sem.phase == Phase::Red ? cfg.red_yellow_duration : 0.0f);
        if (stopped) {
            int gs = (int)std::lround(green_in);
            return {AdviceLevel::Info, FormatIntText("等待绿灯 ", gs, " s"), 0.0f, false};
        }
        if (eta >= green_in) {
            return {AdviceLevel::Go, "保持车速，抵达时为绿灯", 0.0f, false};
        }
        float v_target = (green_in > 0.1f) ? (d / green_in) : vmax;
        if (v_target * 3.6f >= cfg.min_glide_speed_kmh) {
            float clamped = std::min(v_target, vmax) * 3.6f;
            return {AdviceLevel::Adjust, FormatIntText("减速至 ", (int)std::lround(v_target * 3.6f), " km/h 绿波通过"), clamped, true};
        }
        if (stop_decel > cfg.max_decel) {
            return {AdviceLevel::Warn, "红灯！紧急制动", 0.0f, false};
        }
        int dm = (int)std::lround(d);
        if (stop_decel > cfg.comfort_decel * 0.6f) {
            return {AdviceLevel::Stop, FormatIntText("红灯，立即减速（", dm, " m）"), 0.0f, false};
        }
        return {AdviceLevel::Stop, FormatIntText("红灯，前方 ", dm, " m 停车"), 0.0f, false};
    }
    case Phase::Flashing:
        return {AdviceLevel::Warn, "黄灯闪烁，减速观察让行", 0.0f, false};
    default:
        return {AdviceLevel::Info, "信号灯状态未知", 0.0f, false};
    }
}

HudSnapshot BuildDemoSnapshot(uint64_t now_ms, const AdvisorConfig& cfg) {
    struct Stage {
        Phase phase;
        float duration;
        float start_t;
        float dist;
        float speed_kmh;
    };
    static const Stage stages[] = {
        {Phase::Red, 5.0f, 15.0f, 128.0f, 52.0f},
        {Phase::Green, 4.0f, 3.4f, 86.0f, 64.0f},
        {Phase::Green, 4.0f, 14.0f, 95.0f, 58.0f},
        {Phase::Yellow, 3.0f, 2.1f, 24.0f, 48.0f},
        {Phase::RedYellow, 2.0f, 2.0f, 14.0f, 0.0f},
    };
    uint64_t cycle_ms = 18000ULL;
    float elapsed = (float)((now_ms % cycle_ms) / 1000.0);
    const Stage* cur = &stages[0];
    float local_t = elapsed;
    for (const auto& st : stages) {
        if (local_t < st.duration) {
            cur = &st;
            break;
        }
        local_t -= st.duration;
    }

    TargetResult tgt;
    tgt.sem.type = TYPE_TRAFFIC_LIGHT;
    tgt.sem.phase = cur->phase;
    tgt.sem.time_left = std::max(0.2f, cur->start_t - local_t);
    tgt.distance = std::max(2.0f, cur->dist - (cur->speed_kmh / 3.6f) * local_t * 0.4f);

    TruckTelemetry tr;
    tr.speed = cur->speed_kmh / 3.6f;
    tr.speed_limit = 70.0f / 3.6f;
    AdviceResult adv = ComputeAdvice(tgt, tr, cfg);

    HudSnapshot snap;
    snap.visible = true;
    snap.phase = cur->phase;
    snap.is_gate = false;
    snap.time_left = tgt.sem.time_left;
    snap.distance = tgt.distance;
    snap.advice_level = adv.level;
    snap.advice_text = adv.text;
    snap.is_demo = true;
    return snap;
}

} // namespace tlhud
