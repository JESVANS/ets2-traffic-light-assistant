#pragma once

#include "types.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace tlhud {

class HudPainter {
public:
    static constexpr int kWidth = 760;
    static constexpr int kHeight = 280;

    HudPainter();
    ~HudPainter();

    HudPainter(const HudPainter&) = delete;
    HudPainter& operator=(const HudPainter&) = delete;

    // Ensures GDI+ is initialized for the current process (idempotent).
    static bool InitGdiPlus();
    static void ShutdownGdiPlus();

    // Updates the cached BGRA premultiplied buffer only when visual content changes.
    // Returns true if pixels were repainted (texture needs upload).
    bool Update(const HudSnapshot& snap, float bg_opacity, uint64_t now_ms);

    const uint8_t* Pixels() const { return pixels_.data(); }
    int StrideBytes() const { return kWidth * 4; }
    bool IsDirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; }

    // Saves current premultiplied BGRA buffer as a PNG file (for offline visual testing).
    bool SavePng(const std::wstring& path) const;

private:
    void Render(const HudSnapshot& snap, float bg_opacity, bool blink_on);

    std::vector<uint8_t> pixels_;
    bool dirty_ = true;
    bool has_last_ = false;
    uint64_t last_key_ = 0;
    std::string last_advice_;
    std::string last_toast_;
    float last_bg_opacity_ = -1.0f;
};

} // namespace tlhud
