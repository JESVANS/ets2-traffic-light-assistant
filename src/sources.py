"""
数据源：GameSource 从游戏共享内存读取；DemoSource 生成模拟数据用于无游戏预览。
"""
import math
import time
from dataclasses import dataclass, field
from typing import List, Optional

from .semaphores import (Semaphore, Phase, TYPE_TRAFFIC_LIGHT, parse_semaphores)
from .shm import SharedMemoryReader
from .telemetry import TelemetryReader, TruckState

OK = "ok"


@dataclass
class Snapshot:
    status: str                       # OK 或错误提示文本
    truck: Optional[TruckState] = None
    semaphores: List[Semaphore] = field(default_factory=list)


class GameSource:
    def __init__(self, cfg):
        self.cfg = cfg
        self.telemetry = TelemetryReader(cfg.source.scs_shm_name)
        self.sem_reader = SharedMemoryReader(cfg.source.semaphore_shm_name)
        self._last_time = None
        self._last_change = time.monotonic()

    def poll(self) -> Snapshot:
        try:
            truck = self.telemetry.read()
        except Exception as e:
            return Snapshot(f"遥测读取失败: {e}")
        if truck is None:
            return Snapshot("等待游戏启动（未检测到 scs-telemetry）")

        now = time.monotonic()
        if truck.game_time != self._last_time:
            self._last_time = truck.game_time
            self._last_change = now
        elif not truck.paused and now - self._last_change > self.cfg.source.stale_timeout:
            return Snapshot("游戏未运行", truck)
        if truck.paused:
            return Snapshot("游戏已暂停", truck)

        if not self.sem_reader.open():
            return Snapshot("未检测到 ets2la_plugin 红绿灯数据", truck)
        raw = self.sem_reader.read()
        sems = parse_semaphores(raw, self.cfg.source, self.cfg.states)
        return Snapshot(OK, truck, sems)

    def close(self):
        self.telemetry.close()
        self.sem_reader.close()


class DemoSource:
    """模拟：卡车沿 -Z 方向行驶，前方 350 m 处有一个循环的红绿灯"""
    CYCLE = [(Phase.GREEN, 14.0), (Phase.YELLOW, 3.0), (Phase.RED, 18.0), (Phase.RED_YELLOW, 2.0)]

    def __init__(self, cfg):
        self.cfg = cfg
        self.t0 = time.monotonic()
        self.last = self.t0
        self.z = 0.0
        self.light_z = -350.0
        self.speed = 60 / 3.6
        self.target_speed = self.speed
        self.stop_requested = False

    def _light(self, now):
        total = sum(d for _, d in self.CYCLE)
        t = (now - self.t0) % total
        for phase, dur in self.CYCLE:
            if t < dur:
                return phase, dur - t
            t -= dur
        return Phase.GREEN, 0.0

    def feedback(self, advice, target):
        """根据建议简单模拟驾驶员操作"""
        if advice is None:
            self.target_speed, self.stop_requested = 60 / 3.6, False
            return
        self.stop_requested = advice.level in ("stop", "warn") and target.sem.phase != Phase.GREEN \
            and "通过" not in advice.text
        if advice.target_kmh:
            self.target_speed = advice.target_kmh / 3.6
        elif advice.level == "go":
            self.target_speed = 60 / 3.6

    def poll(self) -> Snapshot:
        now = time.monotonic()
        dt, self.last = now - self.last, now
        phase, left = self._light(now)
        dist = self.z - self.light_z - 4.0
        if self.stop_requested and dist > 0:
            decel = min(4.0, self.speed ** 2 / (2 * max(dist, 0.5)))
            self.speed = max(0.0, self.speed - decel * dt)
        else:
            diff = self.target_speed - self.speed
            self.speed += max(-2.0 * dt, min(0.8 * dt, diff))
        self.z -= self.speed * dt
        if self.z < self.light_z - 30:
            self.z = 0.0
        truck = TruckState(True, False, int(now * 1e6), self.speed, False, 0.0, 70 / 3.6,
                           0.0, 0.0, self.z, 0.0)
        code = {Phase.GREEN: self.cfg.states.green, Phase.YELLOW: self.cfg.states.yellow,
                Phase.RED: self.cfg.states.red, Phase.RED_YELLOW: self.cfg.states.red_yellow}[phase]
        sem = Semaphore(0, TYPE_TRAFFIC_LIGHT, code, phase, left, 1,
                        3.0, 0.0, self.light_z, 1.0, 0.0, 0.0, 0.0)
        return Snapshot(OK, truck, [sem])

    def close(self):
        pass
