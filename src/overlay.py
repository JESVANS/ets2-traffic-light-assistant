"""
透明置顶悬浮窗（鼠标穿透），显示红绿灯状态、倒计时、距离和巡航建议。
注意：游戏需使用「窗口化全屏 / 无边框」模式，独占全屏下外部悬浮窗不可见。
"""
import ctypes
import time
from dataclasses import dataclass
from typing import Optional

from PyQt5.QtCore import Qt, QRectF, QPoint
from PyQt5.QtGui import QColor, QFont, QPainter, QPen, QPixmap, QIcon
from PyQt5.QtWidgets import QWidget, QSystemTrayIcon, QMenu, QAction, QApplication

from .advisor import Advice, GO, ADJUST, STOP, WARN, INFO
from .semaphores import Phase, PHASE_NAMES

GWL_EXSTYLE = -20
WS_EX_LAYERED = 0x00080000
WS_EX_TRANSPARENT = 0x00000020
WS_EX_TOOLWINDOW = 0x00000080
WS_EX_NOACTIVATE = 0x08000000

_user32 = ctypes.windll.user32
_user32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
_user32.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
_user32.SetWindowLongPtrW.restype = ctypes.c_ssize_t
_user32.SetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_ssize_t]

RED = QColor(255, 55, 55)
YELLOW = QColor(255, 190, 20)
GREEN = QColor(50, 230, 100)
DIM = QColor(55, 58, 64)
WHITE = QColor(235, 238, 242)
GREY = QColor(150, 155, 165)

LEVEL_COLORS = {GO: GREEN, ADJUST: QColor(80, 200, 255), STOP: QColor(255, 150, 40),
                WARN: RED, INFO: WHITE}

PHASE_COLORS = {Phase.RED: RED, Phase.RED_YELLOW: RED, Phase.GREEN: GREEN,
                Phase.YELLOW: YELLOW, Phase.FLASHING: YELLOW,
                Phase.GATE_CLOSED: RED, Phase.GATE_CLOSING: YELLOW,
                Phase.GATE_OPENING: YELLOW, Phase.GATE_OPEN: GREEN}


@dataclass
class ViewModel:
    status: str = ""                 # 非空时仅显示状态条
    phase: Optional[Phase] = None
    is_gate: bool = False
    time_left: float = 0.0
    distance: float = 0.0
    speed_kmh: float = 0.0
    limit_kmh: float = 0.0
    advice: Optional[Advice] = None


def make_icon() -> QIcon:
    pm = QPixmap(64, 64)
    pm.fill(Qt.transparent)
    p = QPainter(pm)
    p.setRenderHint(QPainter.Antialiasing)
    p.setBrush(QColor(30, 30, 30))
    p.setPen(Qt.NoPen)
    p.drawRoundedRect(16, 2, 32, 60, 8, 8)
    for i, c in enumerate((RED, YELLOW, GREEN)):
        p.setBrush(c)
        p.drawEllipse(22, 6 + i * 19, 20, 17)
    p.end()
    return QIcon(pm)


class Overlay(QWidget):
    BASE_W, BASE_H = 380, 150

    def __init__(self, cfg, app_cfg):
        super().__init__(None, Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.Tool)
        self.cfg = cfg
        self.app_cfg = app_cfg
        self.s = cfg.scale
        self.model = ViewModel(status="初始化中…")
        self.locked = True
        self._drag_offset = None
        self.setAttribute(Qt.WA_TranslucentBackground)
        self.setAttribute(Qt.WA_ShowWithoutActivating)
        self.setFixedSize(int(self.BASE_W * self.s), int(self.BASE_H * self.s))
        self.move(cfg.overlay_x, cfg.overlay_y)
        self._build_tray()

    # ---------- 窗口属性 ----------
    def showEvent(self, e):
        super().showEvent(e)
        self._apply_click_through(self.locked)

    def _apply_click_through(self, enable: bool):
        hwnd = int(self.winId())
        style = _user32.GetWindowLongPtrW(hwnd, GWL_EXSTYLE)
        style |= WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
        style = style | WS_EX_TRANSPARENT if enable else style & ~WS_EX_TRANSPARENT
        _user32.SetWindowLongPtrW(hwnd, GWL_EXSTYLE, style)

    def _build_tray(self):
        self.tray = QSystemTrayIcon(make_icon(), self)
        self.tray.setToolTip("ETS2 红绿灯巡航辅助")
        menu = QMenu()
        self.act_toggle = QAction("隐藏悬浮窗", menu, triggered=self._toggle_visible)
        self.act_lock = QAction("解锁位置（可拖动）", menu, triggered=self._toggle_lock)
        act_quit = QAction("退出", menu, triggered=QApplication.instance().quit)
        for a in (self.act_toggle, self.act_lock):
            menu.addAction(a)
        menu.addSeparator()
        menu.addAction(act_quit)
        self.tray.setContextMenu(menu)
        self._menu = menu
        self.tray.show()

    def _toggle_visible(self):
        if self.isVisible():
            self.hide()
            self.act_toggle.setText("显示悬浮窗")
        else:
            self.show()
            self.act_toggle.setText("隐藏悬浮窗")

    def _toggle_lock(self):
        self.locked = not self.locked
        self._apply_click_through(self.locked)
        if self.locked:
            self.act_lock.setText("解锁位置（可拖动）")
            self.app_cfg.save_value("Overlay", "overlay_x", self.x())
            self.app_cfg.save_value("Overlay", "overlay_y", self.y())
        else:
            self.act_lock.setText("锁定位置并保存")
        self.update()

    def mousePressEvent(self, e):
        if not self.locked and e.button() == Qt.LeftButton:
            self._drag_offset = e.globalPos() - self.frameGeometry().topLeft()

    def mouseMoveEvent(self, e):
        if self._drag_offset is not None:
            self.move(e.globalPos() - self._drag_offset)

    def mouseReleaseEvent(self, e):
        self._drag_offset = None

    # ---------- 绘制 ----------
    def set_model(self, model: ViewModel):
        self.model = model
        self.update()

    def _font(self, pt, bold=False):
        f = QFont("Microsoft YaHei UI", max(1, int(pt * self.s)))
        f.setBold(bold)
        return f

    def paintEvent(self, _):
        p = QPainter(self)
        p.setRenderHint(QPainter.Antialiasing)
        p.setRenderHint(QPainter.TextAntialiasing)
        s, m = self.s, self.model
        bg = QColor(14, 17, 23, int(255 * self.cfg.opacity))

        status_only = bool(m.status) or m.phase is None
        if status_only and self.cfg.hide_when_no_light and self.locked:
            self._draw_status_bar(p, bg, m.status or "前方无红绿灯", m.speed_kmh)
        elif status_only:
            p.setPen(Qt.NoPen)
            p.setBrush(bg)
            p.drawRoundedRect(QRectF(0, 0, self.width(), self.height()), 12 * s, 12 * s)
            self._draw_lamp(p, None)
            p.setPen(GREY)
            p.setFont(self._font(13))
            p.drawText(QRectF(70 * s, 0, 300 * s, self.height()), Qt.AlignVCenter,
                       m.status or "前方无红绿灯")
        else:
            self._draw_full(p, bg, m)

        if not self.locked:
            p.setPen(QPen(QColor(80, 200, 255), 2, Qt.DashLine))
            p.setBrush(Qt.NoBrush)
            p.drawRoundedRect(QRectF(1, 1, self.width() - 2, self.height() - 2), 12 * s, 12 * s)
        p.end()

    def _draw_status_bar(self, p, bg, text, speed):
        s = self.s
        p.setPen(Qt.NoPen)
        p.setBrush(bg)
        p.drawRoundedRect(QRectF(0, 0, 230 * s, 30 * s), 8 * s, 8 * s)
        p.setBrush(GREY)
        p.drawEllipse(QRectF(10 * s, 10 * s, 10 * s, 10 * s))
        p.setPen(GREY)
        p.setFont(self._font(9))
        p.drawText(QRectF(28 * s, 0, 200 * s, 30 * s), Qt.AlignVCenter, text)

    def _draw_lamp(self, p, phase):
        s = self.s
        p.setPen(Qt.NoPen)
        p.setBrush(QColor(22, 26, 33, 210))
        p.drawRoundedRect(QRectF(10 * s, 9 * s, 50 * s, 132 * s), 11 * s, 11 * s)
        blink_on = int(time.monotonic() * 2) % 2 == 0
        lit = {
            Phase.RED: {0}, Phase.RED_YELLOW: {0, 1}, Phase.YELLOW: {1},
            Phase.GREEN: {2}, Phase.FLASHING: {1} if blink_on else set(),
            Phase.GATE_CLOSED: {0}, Phase.GATE_CLOSING: {1}, Phase.GATE_OPENING: {1},
            Phase.GATE_OPEN: {2},
        }.get(phase, set())
        for i, c in enumerate((RED, YELLOW, GREEN)):
            rect = QRectF(17 * s, (15 + i * 42) * s, 36 * s, 36 * s)
            if i in lit:
                glow = QColor(c)
                glow.setAlpha(95)
                p.setBrush(glow)
                p.drawEllipse(rect.adjusted(-4 * s, -4 * s, 4 * s, 4 * s))
                p.setBrush(c)
            else:
                p.setBrush(DIM)
            p.drawEllipse(rect)

    def _draw_full(self, p, bg, m: ViewModel):
        s = self.s
        p.setPen(Qt.NoPen)
        p.setBrush(bg)
        p.drawRoundedRect(QRectF(0, 0, self.width(), self.height()), 12 * s, 12 * s)
        self._draw_lamp(p, m.phase)

        color = PHASE_COLORS.get(m.phase, WHITE)
        x0 = 74 * s
        # 倒计时（纯数字，无下方汉字说明）
        t = max(0.0, m.time_left)
        countdown = f"{t:.1f}" if t < 5 else f"{int(t + 0.999)}"
        show_timer = m.phase not in (Phase.FLASHING, Phase.GATE_OPEN, Phase.GATE_CLOSED) and t < 999
        p.setPen(color)
        if show_timer:
            p.setFont(self._font(44, True))
            p.drawText(QRectF(x0, 4 * s, 165 * s, 76 * s), Qt.AlignLeft | Qt.AlignVCenter, countdown)
        else:
            p.setFont(self._font(26, True))
            p.drawText(QRectF(x0, 4 * s, 165 * s, 76 * s), Qt.AlignLeft | Qt.AlignVCenter,
                       PHASE_NAMES.get(m.phase, ""))

        # 右侧：放大距离（去掉灰色车速/限速小字）
        xr = 220 * s
        p.setPen(WHITE)
        p.setFont(self._font(30, True))
        p.drawText(QRectF(xr, 4 * s, 146 * s, 76 * s), Qt.AlignRight | Qt.AlignVCenter,
                   f"{m.distance:.0f} m")

        # 分隔线
        p.setPen(QPen(QColor(255, 255, 255, 45), 1))
        p.drawLine(int(x0), int(84 * s), int(self.width() - 14 * s), int(84 * s))

        # 巡航建议（放大并保持空间协调）
        if m.advice:
            ac = LEVEL_COLORS.get(m.advice.level, WHITE)
            p.setPen(Qt.NoPen)
            p.setBrush(ac)
            p.drawRoundedRect(QRectF(x0, 98 * s, 6 * s, 38 * s), 3 * s, 3 * s)
            p.setPen(ac)
            p.setFont(self._font(17, True))
            p.drawText(QRectF(x0 + 14 * s, 90 * s, self.width() - x0 - 26 * s, 52 * s),
                       Qt.AlignLeft | Qt.AlignVCenter | Qt.TextWordWrap, m.advice.text)
