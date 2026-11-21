#pragma once

#include "hud_math.hpp"
#include "hud_painter.hpp"

namespace tlhud {

using FrameRenderCallback = bool (*)(float vp_width,
                                     float vp_height,
                                     HudVertex out_verts[4],
                                     HudPainter& painter,
                                     void* user);

bool StartD3D11Hook(FrameRenderCallback cb, void* user);
void StopD3D11Hook();

} // namespace tlhud
