#pragma once

#include "types.hpp"
#include <cstdint>

namespace tlhud {

struct Vec3f {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec3d {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct Quatf {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct HudVertex {
    float x, y, z, w;
    float u, v;
    float alpha;
};

Quatf QuatNormalize(const Quatf& q);
Vec3f QuatRotate(const Vec3f& v, const Quatf& q);
Vec3f QuatRotateInv(const Vec3f& v, const Quatf& q);

class CabAnchorTracker {
public:
    void Reset();

    // Evaluates camera vs truck pose, updates cab anchor, and reports whether camera is inside cab.
    bool Update(const CameraMemData& cam, double sector_size, uint64_t now_ms, bool& out_in_cab);

    bool HasAnchor() const { return has_anchor_; }
    Vec3f Anchor() const { return cab_eye_; }
    Vec3f LastCamLocal() const { return last_cam_local_; }

private:
    bool has_anchor_ = false;
    bool rough_only_ = true;
    Vec3f cab_eye_{};
    Vec3f last_cam_local_{};
    bool has_prev_truck_ = false;
    Vec3d last_truck_world_{};
    uint64_t forward_stable_since_ms_ = 0;
    uint64_t relock_since_ms_ = 0;
};

// Projects the 3D windshield HUD quad into D3D11 clip space (4 vertices in TRIANGLESTRIP order: TL, TR, BL, BR).
bool ProjectHudQuad(const CameraMemData& cam,
                    const Vec3f& cab_eye,
                    const HudConfig& cfg,
                    double sector_size,
                    float vp_width,
                    float vp_height,
                    float alpha,
                    HudVertex out_verts[4]);

// Builds a 2D screen-space quad (TRIANGLESTRIP order: TL, TR, BL, BR) centered at (screen_x, screen_y).
void BuildScreen2DQuad(const HudConfig& cfg,
                       float vp_width,
                       float vp_height,
                       float canvas_aspect_h_over_w,
                       float alpha,
                       HudVertex out_verts[4]);

} // namespace tlhud
