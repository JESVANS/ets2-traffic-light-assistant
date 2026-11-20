"""
ETS2 红绿灯巡航辅助 —— 主程序
用法：
    python main.py            连接游戏运行
    python main.py --demo     使用模拟数据预览悬浮窗
"""
import argparse
import signal
import sys
import threading
import time

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import QApplication

from src.advisor import Advisor, WARN, STOP
from src.config import AppConfig
from src.overlay import Overlay, ViewModel
from src.semaphores import TargetSelector, Phase, TYPE_GATE
from src.sources import GameSource, DemoSource, OK


def _beep(pattern):
    try:
        import winsound
        for freq, dur in pattern:
            winsound.Beep(freq, dur)
            time.sleep(0.05)
    except Exception:
        pass


class Controller:
    def __init__(self, cfg: AppConfig, source, overlay: Overlay):
        self.cfg = cfg
        self.source = source
        self.overlay = overlay
        self.selector = TargetSelector(cfg.selection)
        self.advisor = Advisor(cfg.advisor)
        self._last_alert = (None, None)
        self._last_alert_time = 0.0
        self.timer = QTimer()
        self.timer.timeout.connect(self.tick)
        self.timer.start(max(20, int(1000 / max(1, cfg.general.poll_hz))))

    def tick(self):
        snap = self.source.poll()
        feedback = getattr(self.source, "feedback", None)
        if snap.status != OK:
            self.overlay.set_model(ViewModel(status=snap.status))
            return
        truck = snap.truck
        target = self.selector.select(truck, snap.semaphores)
        if target is None:
            self.overlay.set_model(ViewModel(speed_kmh=truck.speed_kmh))
            if feedback:
                feedback(None, None)
            return
        advice = self.advisor.advise(target, truck)
        if feedback:
            feedback(advice, target)
        self.overlay.set_model(ViewModel(
            phase=target.sem.phase,
            is_gate=target.sem.type == TYPE_GATE,
            time_left=target.sem.time_left,
            distance=target.distance,
            speed_kmh=truck.speed_kmh,
            limit_kmh=truck.speed_limit * 3.6,
            advice=advice,
        ))
        self._maybe_alert(advice, target)

    def _maybe_alert(self, advice, target):
        if not self.cfg.overlay.sound_alert:
            return
        dangerous = target.sem.phase in (Phase.RED, Phase.YELLOW, Phase.GATE_CLOSED, Phase.GATE_CLOSING)
        if advice.level == WARN and dangerous:
            pattern = [(1400, 120)] * 3
        elif advice.level == STOP and dangerous and target.distance < 150:
            pattern = [(900, 180)]
        else:
            return
        key = (target.sem.key, advice.level)
        now = time.monotonic()
        if key == self._last_alert or now - self._last_alert_time < 4.0:
            return
        self._last_alert, self._last_alert_time = key, now
        threading.Thread(target=_beep, args=(pattern,), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description="ETS2 红绿灯巡航辅助")
    parser.add_argument("--demo", action="store_true", help="使用模拟数据预览")
    parser.add_argument("--config", default=None, help="配置文件路径")
    args = parser.parse_args()

    signal.signal(signal.SIGINT, signal.SIG_DFL)
    QApplication.setAttribute(Qt.AA_EnableHighDpiScaling)
    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)

    cfg = AppConfig(args.config)
    source = DemoSource(cfg) if args.demo else GameSource(cfg)
    overlay = Overlay(cfg.overlay, cfg)
    overlay.show()
    controller = Controller(cfg, source, overlay)  # noqa: F841
    print("ETS2 红绿灯巡航辅助已启动（右键托盘图标可解锁位置 / 退出）")
    code = app.exec_()
    source.close()
    sys.exit(code)


if __name__ == "__main__":
    main()
