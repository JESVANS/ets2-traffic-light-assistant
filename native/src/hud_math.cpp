#include "hud_math.hpp"

#include <cmath>

namespace tlhud {

namespace {

constexpr float kPi = 3.14159265358979323846f;

bool IsFiniteCam(const CameraMemData& c) {
    return std::isfinite(c.fov) && std::isfinite(c.pos_x) && std::isfinite(c.pos_y) &&
           std::isfinite(c.pos_z) && std::isfinite(c.qw) && std::isfinite(c.qx) &&
           std::isfinite(c.qy) && std::isfinite(c.qz) && std::isfinite(c.truck_pos_x) &&
           std::isfinite(c.truck_pos_y) && std::isfinite(c.truck_pos_z) &&
           std::isfinite(c.truck_rot_w) && std::isfinite(c.truck_rot_x) &&
           std::isfinite(c.truck_rot_y) && std::isfinite(c.truck_rot_z);
}

bool IsInsideRoughCabBox(const Vec3f& p) {
    // Covers compact cars, short/long-nose trucks, 12-15m coaches, double-deckers, and 18m articulated buses
    return std::fabs(p.x) <= 1.65f &&
           p.y >= -1.80f && p.y <= 2.80f &&
           p.z >= -9.50f && p.z <= 3.50f;
}

float Dist3(const Vec3f& a, const Vec3f& b) {
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool ProjectViewPoint(const CameraMemData& cam,
                      const Vec3f& v,
                      float vp_width,
                      float vp_height,
                      float& out_cx,
                      float& out_cy,
                      float& out_cw) {
    const float* m = cam.m;
    bool finite_m = true;
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) {
            finite_m = false;
            break;
        }
    }

    // Check if projection matrix looks populated
    if (finite_m && std::fabs(m[0]) > 1e-4f && std::fabs(m[5]) > 1e-4f) {
        if (std::fabs(m[14]) > 0.5f) {
            // Row-major left-multiply: clip = M * (v, 1)
            out_cx = m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3];
            out_cy = m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7];
            out_cw = m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15];
            return true;
        }
        if (std::fabs(m[11]) > 0.5f) {
            // Column-major / row-vector multiply
            out_cx = m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12];
            out_cy = m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13];
            out_cw = m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15];
            return true;
        }
    }

    // Fallback using camera horizontal FOV & viewport aspect
    float fov_deg = (cam.fov > 10.0f && cam.fov < 170.0f) ? cam.fov : 65.0f;
    float aspect = (vp_height > 1.0f) ? (vp_width / vp_height) : (16.0f / 9.0f);
    float fx = 1.0f / std::tan((fov_deg * 0.5f) * (kPi / 180.0f));
    float fy = fx * aspect;
    out_cx = v.x * fx;
    out_cy = v.y * fy;
    out_cw = -v.z;
    return true;
}

} // namespace

Quatf QuatNormalize(const Quatf& q) {
    float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-6f) return {1.0f, 0.0f, 0.0f, 0.0f};
    float inv = 1.0f / n;
    return {q.w * inv, q.x * inv, q.y * inv, q.z * inv};
}

Vec3f QuatRotate(const Vec3f& v, const Quatf& q) {
    float x2 = q.x + q.x;
    float y2 = q.y + q.y;
    float z2 = q.z + q.z;

    float wx2 = q.w * x2;
    float wy2 = q.w * y2;
    float wz2 = q.w * z2;
    float xx2 = q.x * x2;
    float xy2 = q.x * y2;
    float xz2 = q.x * z2;
    float yy2 = q.y * y2;
    float yz2 = q.y * z2;
    float zz2 = q.z * z2;

    return {
        v.x * (1.0f - yy2 - zz2) + v.y * (xy2 - wz2) + v.z * (xz2 + wy2),
        v.x * (xy2 + wz2) + v.y * (1.0f - xx2 - zz2) + v.z * (yz2 - wx2),
        v.x * (xz2 - wy2) + v.y * (yz2 + wx2) + v.z * (1.0f - xx2 - yy2),
    };
}

Vec3f QuatRotateInv(const Vec3f& v, const Quatf& q) {
    return QuatRotate(v, {q.w, -q.x, -q.y, -q.z});
}

void CabAnchorTracker::Reset() {
    has_anchor_ = false;
    rough_only_ = true;
    cab_eye_ = {};
    forward_stable_since_ms_ = 0;
    relock_since_ms_ = 0;
}

bool CabAnchorTracker::Update(const CameraMemData& cam, double sector_size, uint64_t now_ms, bool& out_in_cab) {
    out_in_cab = false;
    if (!IsFiniteCam(cam)) return false;

    Quatf truck_rot = QuatNormalize({cam.truck_rot_w, cam.truck_rot_x, cam.truck_rot_y, cam.truck_rot_z});
    Quatf cam_rot = QuatNormalize({cam.qw, cam.qx, cam.qy, cam.qz});

    // Check if camera data is actually initialized (non-zero orientation/position)
    if (std::fabs(cam.qw) + std::fabs(cam.qx) + std::fabs(cam.qy) + std::fabs(cam.qz) < 1e-4f) {
        return false;
    }

    // Detect vehicle switch / garage teleport (>50m jump) and immediately recalibrate cab eye
    Vec3d cur_truck{cam.truck_pos_x, cam.truck_pos_y, cam.truck_pos_z};
    if (has_prev_truck_) {
        double tdx = cur_truck.x - last_truck_world_.x;
        double tdy = cur_truck.y - last_truck_world_.y;
        double tdz = cur_truck.z - last_truck_world_.z;
        if (tdx * tdx + tdy * tdy + tdz * tdz > 50.0 * 50.0) {
            Reset();
        }
    }
    last_truck_world_ = cur_truck;
    has_prev_truck_ = true;

    Vec3d cam_world{
        (double)cam.pos_x + (double)cam.cx * sector_size,
        (double)cam.pos_y,
        (double)cam.pos_z + (double)cam.cz * sector_size
    };
    Vec3f dp_world{
        (float)(cam_world.x - (double)cam.truck_pos_x),
        (float)(cam_world.y - (double)cam.truck_pos_y),
        (float)(cam_world.z - (double)cam.truck_pos_z)
    };
    Vec3f cam_local = QuatRotateInv(dp_world, truck_rot);
    last_cam_local_ = cam_local;
    Vec3f cam_fwd_world = QuatRotate({0.0f, 0.0f, -1.0f}, cam_rot);
    Vec3f cam_fwd_local = QuatRotateInv(cam_fwd_world, truck_rot);

    bool in_rough = IsInsideRoughCabBox(cam_local);
    bool looking_forward = (cam_fwd_local.z < -0.90f);
    bool strictly_forward = (std::fabs(cam_fwd_local.x) < 0.12f && cam_fwd_local.z < -0.92f);

    if (!has_anchor_) {
        if (!in_rough) {
            forward_stable_since_ms_ = 0;
            return true;
        }
        cab_eye_ = cam_local;
        has_anchor_ = true;
        rough_only_ = !looking_forward;
        out_in_cab = true;
        forward_stable_since_ms_ = strictly_forward ? now_ms : 0;
        relock_since_ms_ = 0;
        return true;
    }

    if (rough_only_) {
        if (!in_rough) {
            out_in_cab = false;
            return true;
        }
        cab_eye_ = cam_local;
        if (looking_forward) {
            rough_only_ = false;
        }
        out_in_cab = true;
        return true;
    }

    float dist = Dist3(cam_local, cab_eye_);
    if (dist <= 0.65f) {
        out_in_cab = true;
        relock_since_ms_ = 0;
        if (strictly_forward) {
            if (forward_stable_since_ms_ == 0) {
                forward_stable_since_ms_ = now_ms;
            } else if (now_ms - forward_stable_since_ms_ > 1000) {
                // Smoothly adapt to seat adjustments (F4)
                cab_eye_.x += (cam_local.x - cab_eye_.x) * 0.15f;
                cab_eye_.y += (cam_local.y - cab_eye_.y) * 0.15f;
                cab_eye_.z += (cam_local.z - cab_eye_.z) * 0.15f;
            }
        } else {
            forward_stable_since_ms_ = 0;
        }
        return true;
    }

    forward_stable_since_ms_ = 0;
    if (dist > 0.75f && !in_rough) {
        out_in_cab = false;
        relock_since_ms_ = 0;
        return true;
    }

    // Still inside cab rough box but >0.65m away (e.g., switched truck/seat extreme)
    if (in_rough && looking_forward) {
        if (relock_since_ms_ == 0) {
            relock_since_ms_ = now_ms;
        } else if (now_ms - relock_since_ms_ > 1500) {
            cab_eye_ = cam_local;
            relock_since_ms_ = 0;
            out_in_cab = true;
            return true;
        }
    } else {
        relock_since_ms_ = 0;
    }

    out_in_cab = in_rough && (dist <= 0.75f);
    return true;
}

bool ProjectHudQuad(const CameraMemData& cam,
                    const Vec3f& cab_eye,
                    const HudConfig& cfg,
                    double sector_size,
                    float vp_width,
                    float vp_height,
                    float alpha,
                    HudVertex out_verts[4]) {
    if (!IsFiniteCam(cam)) return false;

    Quatf truck_rot = QuatNormalize({cam.truck_rot_w, cam.truck_rot_x, cam.truck_rot_y, cam.truck_rot_z});
    Quatf cam_rot = QuatNormalize({cam.qw, cam.qx, cam.qy, cam.qz});

    Vec3d cam_world{
        (double)cam.pos_x + (double)cam.cx * sector_size,
        (double)cam.pos_y,
        (double)cam.pos_z + (double)cam.cz * sector_size
    };
    Vec3f truck_rel_cam{
        (float)((double)cam.truck_pos_x - cam_world.x),
        (float)((double)cam.truck_pos_y - cam_world.y),
        (float)((double)cam.truck_pos_z - cam_world.z)
    };

    Vec3f anchor{
        cab_eye.x + cfg.offset_x,
        cab_eye.y + cfg.offset_y,
        cab_eye.z + cfg.offset_z
    };

    float hw = cfg.panel_width * 0.5f;
    float hh = cfg.panel_height * 0.5f;
    float tilt = cfg.tilt_deg * (kPi / 180.0f);
    float ct = std::cos(tilt);
    float st = std::sin(tilt);

    // TRIANGLESTRIP order: TL, TR, BL, BR
    const float lx[4] = {-hw, +hw, -hw, +hw};
    const float ly[4] = {+hh, +hh, -hh, -hh};
    const float us[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    const float vs[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    for (int i = 0; i < 4; ++i) {
        Vec3f p_local{
            anchor.x + lx[i],
            anchor.y + ly[i] * ct,
            anchor.z - ly[i] * st
        };
        Vec3f p_rot = QuatRotate(p_local, truck_rot);
        Vec3f p_world_rel{
            truck_rel_cam.x + p_rot.x,
            truck_rel_cam.y + p_rot.y,
            truck_rel_cam.z + p_rot.z
        };
        Vec3f p_view = QuatRotateInv(p_world_rel, cam_rot);

        float cx = 0.0f, cy = 0.0f, cw = 0.0f;
        if (!ProjectViewPoint(cam, p_view, vp_width, vp_height, cx, cy, cw)) {
            return false;
        }
        if (cw <= 0.15f) {
            return false;
        }
        out_verts[i] = {cx, cy, 0.5f * cw, cw, us[i], vs[i], alpha};
    }
    return true;
}

void BuildScreen2DQuad(const HudConfig& cfg,
                       float vp_width,
                       float vp_height,
                       float canvas_aspect_h_over_w,
                       float alpha,
                       HudVertex out_verts[4]) {
    float aspect = (vp_height > 1.0f) ? (vp_width / vp_height) : (16.0f / 9.0f);
    float w_norm = cfg.screen_width;
    float h_norm = w_norm * aspect * canvas_aspect_h_over_w;

    float left = cfg.screen_x - w_norm * 0.5f;
    float right = cfg.screen_x + w_norm * 0.5f;
    float top = cfg.screen_y - h_norm * 0.5f;
    float bottom = cfg.screen_y + h_norm * 0.5f;

    float ndc_l = left * 2.0f - 1.0f;
    float ndc_r = right * 2.0f - 1.0f;
    float ndc_t = 1.0f - top * 2.0f;
    float ndc_b = 1.0f - bottom * 2.0f;

    // TRIANGLESTRIP: TL, TR, BL, BR
    out_verts[0] = {ndc_l, ndc_t, 0.5f, 1.0f, 0.0f, 0.0f, alpha};
    out_verts[1] = {ndc_r, ndc_t, 0.5f, 1.0f, 1.0f, 0.0f, alpha};
    out_verts[2] = {ndc_l, ndc_b, 0.5f, 1.0f, 0.0f, 1.0f, alpha};
    out_verts[3] = {ndc_r, ndc_b, 0.5f, 1.0f, 1.0f, 1.0f, alpha};
}

} // namespace tlhud
