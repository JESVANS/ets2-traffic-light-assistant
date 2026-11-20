"""
诊断工具：在游戏中停在红绿灯前运行，打印原始数据，用于校验数据结构与参数。
    python probe.py            持续打印（Ctrl+C 退出）
    python probe.py --once     只打印一次
"""
import argparse
import math
import time

from src.config import AppConfig
from src.semaphores import (TargetSelector, relative_geometry, PHASE_NAMES, SEM_SIZE)
from src.sources import GameSource, OK


def dump(src: GameSource, selector: TargetSelector):
    snap = src.poll()
    print("=" * 96)
    if snap.truck:
        t = snap.truck
        print(f"本车: x={t.x:.1f} y={t.y:.1f} z={t.z:.1f} 朝向={math.degrees(t.heading):.1f}° "
              f"车速={t.speed_kmh:.1f}km/h 限速={t.speed_limit * 3.6:.0f} 巡航={t.cruise_on} "
              f"暂停={t.paused}")
    if snap.status != OK:
        print("状态:", snap.status)
        return
    r = src.sem_reader
    print(f"红绿灯共享内存: {r.name} 大小={r.size} 字节 (最多 {r.size // SEM_SIZE} 条)，有效条目={len(snap.semaphores)}")
    target = selector.select(snap.truck, snap.semaphores)
    print(f"{'#':>3} {'类型':<4} {'状态码':>4} {'灯态':<8} {'剩余s':>7} {'纵向m':>8} {'横向m':>7} "
          f"{'高差':>6} {'朝向点积':>8}  id")
    rows = []
    for s in snap.semaphores:
        lon, lat, dy, dot = relative_geometry(snap.truck, s)
        rows.append((math.hypot(lon, lat), s, lon, lat, dy, dot))
    for _, s, lon, lat, dy, dot in sorted(rows, key=lambda r: r[0])[:20]:
        mark = " <== 目标" if target and target.sem.key == s.key else ""
        kind = "灯" if s.type == 1 else "闸"
        print(f"{s.index:>3} {kind:<4} {s.state:>6} {PHASE_NAMES[s.phase]:<8} {s.time_left:>7.1f} "
              f"{lon:>8.1f} {lat:>7.1f} {dy:>6.1f} {dot:>8.2f}  {s.id}{mark}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    args = ap.parse_args()
    cfg = AppConfig()
    src = GameSource(cfg)
    selector = TargetSelector(cfg.selection)
    try:
        while True:
            dump(src, selector)
            if args.once:
                break
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    finally:
        src.close()


if __name__ == "__main__":
    main()
