"""
巡航建议：根据灯态、剩余时间、距离和车速，给出通过 / 调速 / 停车建议。
"""
import math
from dataclasses import dataclass
from typing import Optional

from .semaphores import Phase, Target, TYPE_GATE

GO, ADJUST, STOP, WARN, INFO = "go", "adjust", "stop", "warn", "info"


@dataclass
class Advice:
    level: str
    text: str
    target_kmh: Optional[float] = None


class Advisor:
    def __init__(self, cfg):
        self.cfg = cfg

    def _max_speed(self, truck) -> float:
        """建议车速上限 m/s"""
        c = self.cfg
        if c.use_speed_limit and truck.speed_limit > 1.0:
            return truck.speed_limit
        return c.default_max_speed_kmh / 3.6

    def advise(self, target: Target, truck) -> Advice:
        c = self.cfg
        sem, d = target.sem, target.distance
        v = max(0.0, truck.speed)
        t = max(0.0, sem.time_left)
        vmax = self._max_speed(truck)
        eta = d / v if v > 0.5 else math.inf
        stop_decel = (v * v) / (2 * d) if d > 0.5 else (math.inf if v > 0.5 else 0.0)
        stopped = v < 0.5

        if sem.type == TYPE_GATE:
            if sem.phase == Phase.GATE_OPEN:
                return Advice(GO, "道闸已开启")
            if stopped:
                return Advice(INFO, "等待道闸开启")
            return Advice(STOP, f"道闸未开，前方 {d:.0f} m 停车")

        p = sem.phase
        if p == Phase.GREEN:
            if stopped and d < 15:
                return Advice(GO, "绿灯，起步通过")
            if eta <= t:
                return Advice(GO, "保持车速，可顺利通过")
            if t > 0.5:
                v_need = d / t
                acc_need = (v_need ** 2 - v ** 2) / (2 * d) if d > 0 else math.inf
                if v_need <= vmax and acc_need <= c.max_accel:
                    return Advice(ADJUST, f"加速至 {v_need * 3.6:.0f} km/h 可通过", v_need * 3.6)
            if stop_decel <= c.max_decel:
                return Advice(STOP, "绿灯即将结束，准备减速停车")
            return Advice(WARN, "无法安全停车，谨慎通过")

        if p == Phase.YELLOW:
            if eta <= t + 0.5:
                return Advice(WARN, "黄灯，保持车速通过")
            if stop_decel <= c.max_decel:
                return Advice(STOP, f"黄灯，减速停车（{d:.0f} m）")
            return Advice(WARN, "黄灯，无法安全停车，谨慎通过")

        if p in (Phase.RED, Phase.RED_YELLOW):
            green_in = t + (c.red_yellow_duration if p == Phase.RED else 0.0)
            if stopped:
                return Advice(INFO, f"等待绿灯 {green_in:.0f} s")
            if eta >= green_in:
                return Advice(GO, "保持车速，抵达时为绿灯")
            v_target = d / green_in if green_in > 0.1 else vmax
            if v_target * 3.6 >= c.min_glide_speed_kmh:
                return Advice(ADJUST, f"减速至 {v_target * 3.6:.0f} km/h 绿波通过",
                              min(v_target, vmax) * 3.6)
            if stop_decel > c.max_decel:
                return Advice(WARN, "红灯！紧急制动")
            if stop_decel > c.comfort_decel * 0.6:
                return Advice(STOP, f"红灯，立即减速（{d:.0f} m）")
            return Advice(STOP, f"红灯，前方 {d:.0f} m 停车")

        if p == Phase.FLASHING:
            return Advice(WARN, "黄灯闪烁，减速观察让行")
        return Advice(INFO, "信号灯状态未知")
