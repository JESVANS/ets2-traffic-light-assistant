"""
读取 scs-telemetry.dll（RenCloud / truckermudgeon 版）共享内存中的本车数据。
字段偏移直接复用 truck_telemetry 库的结构定义，每帧只读取所需的几百字节。
"""
import math
from dataclasses import dataclass
from typing import Optional

from .shm import SharedMemoryReader

NEEDED_FIELDS = (
    "sdkActive", "paused", "time", "speed", "cruiseControl", "cruiseControlSpeed",
    "speedLimit", "coordinateX", "coordinateY", "coordinateZ", "rotationX",
)


@dataclass
class TruckState:
    sdk_active: bool
    paused: bool
    game_time: int
    speed: float          # m/s（倒车为负）
    cruise_on: bool
    cruise_speed: float   # m/s
    speed_limit: float    # m/s，0 = 无数据
    x: float
    y: float
    z: float
    heading: float        # 弧度，0 = 朝北（-Z）

    @property
    def forward(self):
        """水平面车头方向单位向量 (x, z)"""
        return -math.sin(self.heading), -math.cos(self.heading)

    @property
    def speed_kmh(self) -> float:
        return abs(self.speed) * 3.6


class TelemetryReader:
    def __init__(self, shm_name: str):
        self.reader = SharedMemoryReader(shm_name)
        self._layout = None
        self._read_len = 0
        self.version_note = ""

    def _init_layout(self):
        from truck_telemetry.telemetry_version import v1_10, v1_12
        buf = self.reader.read()
        chosen = None
        for ver in (v1_12, v1_10):
            try:
                if ver.is_same_version(buf):
                    chosen = ver
                    break
            except Exception:
                pass
        if chosen is None:
            chosen = v1_12
            self.version_note = "未识别的遥测插件版本，按 v1.12 结构解析"
            print(f"[遥测] {self.version_note}")
        fields = chosen.struct_telemetry._DictStruct__fields
        layout, offset = {}, 0
        for name, ftype in fields:
            if name in NEEDED_FIELDS:
                layout[name] = (offset, ftype)
            offset += ftype.size
        missing = set(NEEDED_FIELDS) - set(layout)
        if missing:
            raise RuntimeError(f"遥测结构缺少字段: {missing}")
        self._layout = layout
        self._read_len = max(o + t.size for o, t in layout.values())

    def read(self) -> Optional[TruckState]:
        if not self.reader.open():
            return None
        if self._layout is None:
            self._init_layout()
        buf = self.reader.read(0, self._read_len)
        v = {n: t.unpack_from(buf, o) for n, (o, t) in self._layout.items()}
        return TruckState(
            sdk_active=bool(v["sdkActive"]),
            paused=bool(v["paused"]),
            game_time=int(v["time"]),
            speed=float(v["speed"]),
            cruise_on=bool(v["cruiseControl"]),
            cruise_speed=float(v["cruiseControlSpeed"]),
            speed_limit=float(v["speedLimit"]),
            x=float(v["coordinateX"]),
            y=float(v["coordinateY"]),
            z=float(v["coordinateZ"]),
            heading=float(v["rotationX"]) * 2.0 * math.pi,
        )

    def close(self):
        self.reader.close()
