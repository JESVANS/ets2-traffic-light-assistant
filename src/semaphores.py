"""
解析 ets2la_plugin.dll 输出的红绿灯共享内存 (Local\\ETS2LASemaphore)，
并从中挑选出本车前方、本方向的那一个信号灯。

单条记录格式（插件内格式串 "fffssffffifii"，共 48 字节）：
    x, y, z          float  扇区内局部坐标
    cx, cy           short  扇区号（世界坐标 = 局部 + 扇区号 * 512）
    qw, qx, qy, qz   float  朝向四元数
    type             int    1 = 红绿灯, 2 = 道闸
    time_left        float  当前灯态剩余秒数
    state            int    灯态码（见 config.ini [StateCodes]）
    id               int
"""
import math
import struct
from dataclasses import dataclass
from enum import Enum
from typing import List, Optional

SEM_STRUCT = struct.Struct("<fffhhffffifii")
SEM_SIZE = SEM_STRUCT.size  # 48

TYPE_TRAFFIC_LIGHT = 1
TYPE_GATE = 2


class Phase(Enum):
    UNKNOWN = "unknown"
    OFF = "off"
    RED = "red"
    RED_YELLOW = "red_yellow"
    GREEN = "green"
    YELLOW = "yellow"
    FLASHING = "flashing"
    GATE_CLOSED = "gate_closed"
    GATE_CLOSING = "gate_closing"
    GATE_OPENING = "gate_opening"
    GATE_OPEN = "gate_open"


PHASE_NAMES = {
    Phase.UNKNOWN: "未知", Phase.OFF: "关闭", Phase.RED: "红灯",
    Phase.RED_YELLOW: "红黄灯", Phase.GREEN: "绿灯", Phase.YELLOW: "黄灯",
    Phase.FLASHING: "黄闪", Phase.GATE_CLOSED: "道闸关闭", Phase.GATE_CLOSING: "道闸关闭中",
    Phase.GATE_OPENING: "道闸开启中", Phase.GATE_OPEN: "道闸开启",
}


@dataclass
class Semaphore:
    index: int
    type: int
    state: int
    phase: Phase
    time_left: float
    id: int
    wx: float
    wy: float
    wz: float
    qw: float
    qx: float
    qy: float
    qz: float

    @property
    def key(self):
        return self.id, round(self.wx), round(self.wz)

    def forward_xz(self):
        """信号灯本地 -Z 轴在水平面上的方向"""
        w, x, y, z = self.qw, self.qx, self.qy, self.qz
        fx = -2.0 * (x * z + w * y)
        fz = -(1.0 - 2.0 * (x * x + y * y))
        n = math.hypot(fx, fz)
        return (fx / n, fz / n) if n > 1e-6 else (0.0, 0.0)


def map_phase(sem_type: int, state: int, codes) -> Phase:
    if sem_type == TYPE_GATE:
        return {
            codes.gate_closed: Phase.GATE_CLOSED, codes.gate_closing: Phase.GATE_CLOSING,
            codes.gate_opening: Phase.GATE_OPENING, codes.gate_open: Phase.GATE_OPEN,
        }.get(state, Phase.UNKNOWN)
    return {
        codes.off: Phase.OFF, codes.red: Phase.RED, codes.red_yellow: Phase.RED_YELLOW,
        codes.green: Phase.GREEN, codes.yellow: Phase.YELLOW, codes.flashing: Phase.FLASHING,
    }.get(state, Phase.UNKNOWN)


def parse_semaphores(raw: bytes, source_cfg, codes) -> List[Semaphore]:
    count = len(raw) // SEM_SIZE
    if source_cfg.max_entries > 0:
        count = min(count, source_cfg.max_entries)
    result = []
    sector = source_cfg.sector_size
    for i in range(count):
        x, y, z, cx, cy, qw, qx, qy, qz, typ, tl, state, sid = SEM_STRUCT.unpack_from(raw, i * SEM_SIZE)
        if typ not in (TYPE_TRAFFIC_LIGHT, TYPE_GATE):
            continue
        if not all(math.isfinite(v) for v in (x, y, z, qw, qx, qy, qz, tl)):
            continue
        result.append(Semaphore(
            index=i, type=typ, state=state, phase=map_phase(typ, state, codes),
            time_left=tl, id=sid,
            wx=x + cx * sector, wy=y, wz=z + cy * sector,
            qw=qw, qx=qx, qy=qy, qz=qz,
        ))
    return result


@dataclass
class Target:
    sem: Semaphore
    distance: float        # 到停车线的纵向距离（米）
    longitudinal: float    # 到灯杆的纵向距离
    lateral: float         # 横向偏移


def relative_geometry(truck, sem):
    fx, fz = truck.forward
    dx, dz = sem.wx - truck.x, sem.wz - truck.z
    lon = dx * fx + dz * fz
    lat = dx * (-fz) + dz * fx
    lfx, lfz = sem.forward_xz()
    facing_dot = lfx * fx + lfz * fz
    return lon, lat, sem.wy - truck.y, facing_dot


class TargetSelector:
    def __init__(self, cfg):
        self.cfg = cfg
        self._prev_key = None

    def select(self, truck, sems: List[Semaphore]) -> Optional[Target]:
        c = self.cfg
        best, best_score = None, float("inf")
        for s in sems:
            if s.type == TYPE_GATE and not c.include_gates:
                continue
            if s.phase in (Phase.OFF, Phase.UNKNOWN):
                continue
            lon, lat, dy, dot = relative_geometry(truck, s)
            if lon < 1.0 or lon > c.max_distance:
                continue
            if abs(lat) > c.max_lateral + lon * c.lateral_growth:
                continue
            if abs(dy) > c.max_height_diff:
                continue
            if s.type == TYPE_TRAFFIC_LIGHT:
                thr = c.parallel_threshold
                if c.facing_mode == 0 and abs(dot) < thr:
                    continue
                if c.facing_mode == 1 and dot < thr:
                    continue
                if c.facing_mode == -1 and dot > -thr:
                    continue
            score = lon + c.lateral_weight * abs(lat)
            if s.key == self._prev_key:
                score *= 0.85  # 迟滞，避免目标在相邻灯之间跳动
            if score < best_score:
                best_score = score
                best = Target(sem=s, distance=max(0.0, lon - c.stop_line_offset),
                              longitudinal=lon, lateral=lat)
        self._prev_key = best.sem.key if best else None
        return best
