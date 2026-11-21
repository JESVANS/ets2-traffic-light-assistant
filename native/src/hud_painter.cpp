#include "hud_painter.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// MinGW's gdiplus.h expects min/max macros or std::min/std::max in scope
using std::max;
using std::min;
#include <gdiplus.h>

namespace tlhud {

namespace {

ULONG_PTR g_gdiplus_token = 0;
LONG g_gdiplus_ref = 0;

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), (int)utf8.size(), &out[0], n);
    return out;
}

uint64_t HashString(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r) {
    float d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    if (d <= 0.5f) {
        path.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

Gdiplus::Color PhaseColor(Phase p) {
    switch (p) {
    case Phase::Red:
    case Phase::RedYellow:
    case Phase::GateClosed:
        return Gdiplus::Color(255, 255, 66, 66);
    case Phase::Yellow:
    case Phase::Flashing:
    case Phase::GateClosing:
    case Phase::GateOpening:
        return Gdiplus::Color(255, 255, 198, 26);
    case Phase::Green:
    case Phase::GateOpen:
        return Gdiplus::Color(255, 56, 240, 120);
    default:
        return Gdiplus::Color(255, 235, 240, 245);
    }
}

Gdiplus::Color AdviceColor(AdviceLevel lv) {
    switch (lv) {
    case AdviceLevel::Go:     return Gdiplus::Color(255, 56, 240, 120);
    case AdviceLevel::Adjust: return Gdiplus::Color(255, 74, 216, 255);
    case AdviceLevel::Stop:   return Gdiplus::Color(255, 255, 160, 46);
    case AdviceLevel::Warn:   return Gdiplus::Color(255, 255, 69, 69);
    case AdviceLevel::Info:
    default:                  return Gdiplus::Color(255, 232, 238, 245);
    }
}

const wchar_t* ShortStateLabel(Phase p) {
    switch (p) {
    case Phase::Flashing:    return L"黄闪";
    case Phase::GateOpen:    return L"道闸开";
    case Phase::GateClosed:  return L"道闸关";
    case Phase::GateOpening: return L"道闸开启中";
    case Phase::GateClosing: return L"道闸关闭中";
    case Phase::Off:         return L"关闭";
    default:                 return L"--";
    }
}

bool HasTimer(Phase p, float time_left) {
    if (time_left >= 999.0f || time_left < 0.0f) return false;
    if (p == Phase::Flashing || p == Phase::GateOpen || p == Phase::GateClosed || p == Phase::Off || p == Phase::Unknown) {
        return false;
    }
    return true;
}

std::wstring FormatCountdown(float time_left) {
    float t = std::max(0.0f, time_left);
    wchar_t buf[32];
    if (t < 5.0f) {
        std::swprintf(buf, 32, L"%.1f", (double)t);
    } else {
        int sec = (int)std::ceil(t);
        std::swprintf(buf, 32, L"%d", sec);
    }
    return buf;
}

void DrawTextWithShadow(Gdiplus::Graphics& g,
                        const std::wstring& text,
                        const Gdiplus::Font& font,
                        const Gdiplus::RectF& rect,
                        const Gdiplus::StringFormat& fmt,
                        const Gdiplus::Color& color,
                        float shadow_offset = 2.5f) {
    Gdiplus::RectF sh = rect;
    sh.X += shadow_offset;
    sh.Y += shadow_offset;
    Gdiplus::SolidBrush sh_brush(Gdiplus::Color(185, 0, 0, 0));
    g.DrawString(text.c_str(), -1, &font, sh, &fmt, &sh_brush);

    Gdiplus::SolidBrush fg_brush(color);
    g.DrawString(text.c_str(), -1, &font, rect, &fmt, &fg_brush);
}

bool GetPngClsid(CLSID* out_clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<uint8_t> buf(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, encoders);
    for (UINT i = 0; i < num; ++i) {
        if (std::wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *out_clsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

} // namespace

bool HudPainter::InitGdiPlus() {
    if (InterlockedIncrement(&g_gdiplus_ref) == 1) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &input, nullptr) != Gdiplus::Ok) {
            InterlockedDecrement(&g_gdiplus_ref);
            return false;
        }
    }
    return true;
}

void HudPainter::ShutdownGdiPlus() {
    if (InterlockedDecrement(&g_gdiplus_ref) <= 0) {
        g_gdiplus_ref = 0;
        if (g_gdiplus_token != 0) {
            Gdiplus::GdiplusShutdown(g_gdiplus_token);
            g_gdiplus_token = 0;
        }
    }
}

HudPainter::HudPainter() : pixels_((size_t)kWidth * (size_t)kHeight * 4, 0) {}

HudPainter::~HudPainter() = default;

bool HudPainter::Update(const HudSnapshot& snap, float bg_opacity, uint64_t now_ms) {
    bool blink_on = ((now_ms / 500ULL) % 2ULL) == 0;
    bool timer = HasTimer(snap.phase, snap.time_left);
    int t_bucket = timer ? (snap.time_left < 5.0f ? (int)std::lround(snap.time_left * 10.0f)
                                                  : (int)std::ceil(snap.time_left))
                         : -1;
    int dist_m = (int)std::lround(std::max(0.0f, snap.distance));

    uint64_t key = ((uint64_t)snap.phase
                 | ((uint64_t)snap.advice_level << 8)
                 | ((uint64_t)(blink_on ? 1 : 0) << 16)
                 | ((uint64_t)(uint32_t)(t_bucket + 1) << 20)
                 | ((uint64_t)(uint32_t)dist_m << 40))
                 ^ HashString(snap.advice_text)
                 ^ (HashString(snap.toast_text) * 131ULL);

    if (has_last_ &&
        key == last_key_ &&
        std::fabs(bg_opacity - last_bg_opacity_) < 0.005f &&
        snap.advice_text == last_advice_ &&
        snap.toast_text == last_toast_) {
        return false;
    }

    Render(snap, bg_opacity, blink_on);
    has_last_ = true;
    last_key_ = key;
    last_advice_ = snap.advice_text;
    last_toast_ = snap.toast_text;
    last_bg_opacity_ = bg_opacity;
    dirty_ = true;
    return true;
}

void HudPainter::Render(const HudSnapshot& snap, float bg_opacity, bool blink_on) {
    std::memset(pixels_.data(), 0, pixels_.size());

    Gdiplus::Bitmap bmp(kWidth, kHeight, kWidth * 4, PixelFormat32bppPARGB, pixels_.data());
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

    // 1. Glass background card (skip when bg_opacity <= 0.01 for pure projection look)
    if (bg_opacity > 0.01f) {
        Gdiplus::GraphicsPath card;
        AddRoundedRect(card, 4.0f, 4.0f, kWidth - 8.0f, kHeight - 8.0f, 26.0f);

        int alpha_top = (int)std::clamp(bg_opacity * 255.0f * 1.06f, 0.0f, 255.0f);
        int alpha_bot = (int)std::clamp(bg_opacity * 255.0f * 0.92f, 0.0f, 255.0f);
        Gdiplus::LinearGradientBrush bg_brush(
            Gdiplus::PointF(0.0f, 4.0f),
            Gdiplus::PointF(0.0f, (float)(kHeight - 4)),
            Gdiplus::Color((BYTE)alpha_top, 14, 21, 32),
            Gdiplus::Color((BYTE)alpha_bot, 8, 12, 18)
        );
        g.FillPath(&bg_brush, &card);

        int border_a = (int)std::min(95.0f, bg_opacity * 165.0f + 28.0f);
        Gdiplus::Pen border_pen(Gdiplus::Color((BYTE)border_a, 220, 236, 255), 1.8f);
        g.DrawPath(&border_pen, &card);
    }

    // 2. Left 3-lamp traffic light housing (enlarged & balanced)
    {
        Gdiplus::GraphicsPath box;
        AddRoundedRect(box, 22.0f, 20.0f, 96.0f, 240.0f, 22.0f);
        BYTE box_a = (bg_opacity > 0.01f) ? 155 : 95;
        Gdiplus::SolidBrush box_brush(Gdiplus::Color(box_a, 8, 12, 18));
        g.FillPath(&box_brush, &box);
        Gdiplus::Pen box_pen(Gdiplus::Color(42, 230, 240, 255), 1.4f);
        g.DrawPath(&box_pen, &box);

        bool lit[3] = {false, false, false};
        switch (snap.phase) {
        case Phase::Red:
        case Phase::GateClosed:
            lit[0] = true; break;
        case Phase::RedYellow:
            lit[0] = true; lit[1] = true; break;
        case Phase::Yellow:
        case Phase::GateClosing:
        case Phase::GateOpening:
            lit[1] = true; break;
        case Phase::Flashing:
            lit[1] = blink_on; break;
        case Phase::Green:
        case Phase::GateOpen:
            lit[2] = true; break;
        default: break;
        }

        const Gdiplus::Color lamp_cols[3] = {
            Gdiplus::Color(255, 255, 62, 62),
            Gdiplus::Color(255, 255, 198, 26),
            Gdiplus::Color(255, 56, 240, 120),
        };
        const float cx = 70.0f;
        const float cys[3] = {64.0f, 140.0f, 216.0f};
        const float r = 31.0f;

        for (int i = 0; i < 3; ++i) {
            if (lit[i]) {
                Gdiplus::Color c = lamp_cols[i];
                // Outer & inner soft holographic glow
                Gdiplus::SolidBrush glow1(Gdiplus::Color(46, c.GetR(), c.GetG(), c.GetB()));
                g.FillEllipse(&glow1, cx - (r + 12.0f), cys[i] - (r + 12.0f), (r + 12.0f) * 2.0f, (r + 12.0f) * 2.0f);
                Gdiplus::SolidBrush glow2(Gdiplus::Color(102, c.GetR(), c.GetG(), c.GetB()));
                g.FillEllipse(&glow2, cx - (r + 5.0f), cys[i] - (r + 5.0f), (r + 5.0f) * 2.0f, (r + 5.0f) * 2.0f);

                Gdiplus::SolidBrush core(c);
                g.FillEllipse(&core, cx - r, cys[i] - r, r * 2.0f, r * 2.0f);

                // Subtle specular highlight
                Gdiplus::SolidBrush spec(Gdiplus::Color(90, 255, 255, 255));
                g.FillEllipse(&spec, cx - 15.0f, cys[i] - 21.0f, 30.0f, 13.0f);
            } else {
                Gdiplus::SolidBrush dim(Gdiplus::Color(115, 62, 68, 80));
                g.FillEllipse(&dim, cx - r, cys[i] - r, r * 2.0f, r * 2.0f);
            }
        }
    }

    Gdiplus::FontFamily font_family(L"Microsoft YaHei UI");
    Gdiplus::StringFormat fmt_left(Gdiplus::StringFormat::GenericTypographic());
    fmt_left.SetAlignment(Gdiplus::StringAlignmentNear);
    fmt_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt_left.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::StringFormat fmt_right(Gdiplus::StringFormat::GenericTypographic());
    fmt_right.SetAlignment(Gdiplus::StringAlignmentFar);
    fmt_right.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt_right.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    Gdiplus::StringFormat fmt_center(Gdiplus::StringFormat::GenericTypographic());
    fmt_center.SetAlignment(Gdiplus::StringAlignmentCenter);
    fmt_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    fmt_center.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

    // 3. Top-left: Pure large countdown number (NO Chinese label below!)
    Gdiplus::Color phase_col = PhaseColor(snap.phase);
    if (HasTimer(snap.phase, snap.time_left)) {
        std::wstring cd = FormatCountdown(snap.time_left);
        Gdiplus::Font f_num(&font_family, 108.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::RectF r_num(146.0f, 20.0f, 330.0f, 144.0f);
        DrawTextWithShadow(g, cd, f_num, r_num, fmt_left, phase_col, 3.0f);
    } else {
        std::wstring label = ShortStateLabel(snap.phase);
        Gdiplus::Font f_lbl(&font_family, 58.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::RectF r_lbl(146.0f, 22.0f, 330.0f, 140.0f);
        DrawTextWithShadow(g, label, f_lbl, r_lbl, fmt_left, phase_col, 2.5f);
    }

    // 4. Top-right: Large distance ONLY (NO grey speed/limit text!)
    {
        int dm = std::max(0, (int)std::lround(snap.distance));
        wchar_t dbuf[32];
        std::swprintf(dbuf, 32, L"%d", dm);
        std::wstring dist_num = dbuf;
        std::wstring dist_unit = L"m";

        Gdiplus::Font f_dist(&font_family, 76.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font f_unit(&font_family, 42.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);

        Gdiplus::RectF b_unit;
        g.MeasureString(dist_unit.c_str(), -1, &f_unit, Gdiplus::PointF(0, 0), &fmt_left, &b_unit);

        float unit_w = b_unit.Width;
        float gap = 10.0f;
        float right_edge = 730.0f;

        Gdiplus::RectF r_unit(right_edge - unit_w - 2.0f, 58.0f, unit_w + 8.0f, 82.0f);
        Gdiplus::RectF r_dist(440.0f, 22.0f, (right_edge - unit_w - gap) - 440.0f, 140.0f);

        DrawTextWithShadow(g, dist_num, f_dist, r_dist, fmt_right, Gdiplus::Color(255, 244, 248, 252), 2.5f);
        DrawTextWithShadow(g, dist_unit, f_unit, r_unit, fmt_left, Gdiplus::Color(255, 200, 212, 227), 2.0f);
    }

    // 5. Horizontal glass divider line
    {
        Gdiplus::LinearGradientBrush div_brush(
            Gdiplus::PointF(146.0f, 166.0f),
            Gdiplus::PointF(732.0f, 166.0f),
            Gdiplus::Color(15, 255, 255, 255),
            Gdiplus::Color(70, 255, 255, 255)
        );
        Gdiplus::Pen div_pen(&div_brush, 2.0f);
        g.DrawLine(&div_pen, 146.0f, 166.0f, 732.0f, 166.0f);
    }

    // 6. Bottom-right: Colored status bar + enlarged single-line cruise advice
    {
        Gdiplus::Color ac = AdviceColor(snap.advice_level);
        // Glow behind status pill
        Gdiplus::GraphicsPath pill_glow;
        AddRoundedRect(pill_glow, 143.0f, 188.0f, 16.0f, 62.0f, 7.0f);
        Gdiplus::SolidBrush glow_brush(Gdiplus::Color(65, ac.GetR(), ac.GetG(), ac.GetB()));
        g.FillPath(&glow_brush, &pill_glow);

        Gdiplus::GraphicsPath pill;
        AddRoundedRect(pill, 146.0f, 191.0f, 10.0f, 56.0f, 5.0f);
        Gdiplus::SolidBrush pill_brush(ac);
        g.FillPath(&pill_brush, &pill);

        std::wstring advice_w = Utf8ToWide(snap.advice_text);
        if (!advice_w.empty()) {
            const float avail_w = 558.0f;
            const float sizes[] = {40.0f, 36.0f, 32.0f, 28.0f};
            float chosen = 28.0f;
            for (float sz : sizes) {
                Gdiplus::Font test_font(&font_family, sz, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::RectF box;
                g.MeasureString(advice_w.c_str(), -1, &test_font, Gdiplus::PointF(0, 0), &fmt_left, &box);
                if (box.Width <= avail_w) {
                    chosen = sz;
                    break;
                }
            }
            Gdiplus::Font f_adv(&font_family, chosen, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::RectF r_adv(172.0f, 174.0f, 562.0f, 92.0f);
            DrawTextWithShadow(g, advice_w, f_adv, r_adv, fmt_left, ac, 2.5f);
        }
    }

    // 7. Temporary toast pill during hotkey adjustment
    if (!snap.toast_text.empty()) {
        std::wstring toast_w = Utf8ToWide(snap.toast_text);
        Gdiplus::Font f_toast(&font_family, 18.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::RectF tb;
        g.MeasureString(toast_w.c_str(), -1, &f_toast, Gdiplus::PointF(0, 0), &fmt_left, &tb);
        float pw = std::min(580.0f, tb.Width + 28.0f);
        float px = (kWidth - pw) * 0.5f + 50.0f;
        Gdiplus::GraphicsPath tp;
        AddRoundedRect(tp, px, 8.0f, pw, 28.0f, 14.0f);
        Gdiplus::SolidBrush tbg(Gdiplus::Color(200, 12, 20, 32));
        g.FillPath(&tbg, &tp);
        Gdiplus::Pen tbd(Gdiplus::Color(170, 80, 205, 255), 1.2f);
        g.DrawPath(&tbd, &tp);
        Gdiplus::RectF tr(px, 8.0f, pw, 28.0f);
        DrawTextWithShadow(g, toast_w, f_toast, tr, fmt_center, Gdiplus::Color(255, 195, 238, 255), 1.0f);
    }
}

bool HudPainter::SavePng(const std::wstring& path) const {
    CLSID clsid{};
    if (!GetPngClsid(&clsid)) return false;
    Gdiplus::Bitmap bmp(kWidth, kHeight, kWidth * 4, PixelFormat32bppPARGB,
                        const_cast<uint8_t*>(pixels_.data()));
    return bmp.Save(path.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

} // namespace tlhud
