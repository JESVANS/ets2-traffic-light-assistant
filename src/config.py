"""
配置管理：读取 config.ini（键名与 dataclass 字段名一一对应）
"""
import configparser
import os
import re
from dataclasses import dataclass, fields

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CONFIG_PATH = os.path.join(BASE_DIR, "config.ini")


@dataclass
class GeneralConfig:
    poll_hz: int = 10


@dataclass
class SourceConfig:
    scs_shm_name: str = "Local\\SCSTelemetry"
    semaphore_shm_name: str = "Local\\ETS2LASemaphore"
    sector_size: float = 512.0
    max_entries: int = 0
    stale_timeout: float = 3.0


@dataclass
class StateCodes:
    off: int = 0
    yellow: int = 1
    red: int = 2
    red_yellow: int = 4
    green: int = 8
    flashing: int = 32
    gate_closing: int = 0
    gate_closed: int = 1
    gate_opening: int = 2
    gate_open: int = 3


@dataclass
class SelectionConfig:
    max_distance: float = 300.0
    max_lateral: float = 12.0
    lateral_growth: float = 0.12
    max_height_diff: float = 12.0
    parallel_threshold: float = 0.6
    facing_mode: int = 0
    stop_line_offset: float = 4.0
    lateral_weight: float = 3.0
    include_gates: bool = True


@dataclass
class AdvisorConfig:
    comfort_decel: float = 2.0
    max_decel: float = 4.5
    max_accel: float = 0.6
    min_glide_speed_kmh: float = 20.0
    default_max_speed_kmh: float = 90.0
    red_yellow_duration: float = 2.0
    use_speed_limit: bool = True


@dataclass
class OverlayConfig:
    overlay_x: int = 40
    overlay_y: int = 300
    scale: float = 1.0
    opacity: float = 0.80
    hide_when_no_light: bool = True
    sound_alert: bool = True


def _convert(raw: str, target_type):
    raw = raw.strip()
    if target_type is bool:
        return raw.lower() in ("1", "true", "yes", "on")
    if target_type is int:
        return int(float(raw))
    if target_type is float:
        return float(raw)
    return raw


class AppConfig:
    def __init__(self, path: str = None):
        self.path = path or DEFAULT_CONFIG_PATH
        self.general = GeneralConfig()
        self.source = SourceConfig()
        self.states = StateCodes()
        self.selection = SelectionConfig()
        self.advisor = AdvisorConfig()
        self.overlay = OverlayConfig()
        if os.path.exists(self.path):
            self._load()

    def _sections(self):
        return {
            "General": self.general,
            "Source": self.source,
            "StateCodes": self.states,
            "Selection": self.selection,
            "Advisor": self.advisor,
            "Overlay": self.overlay,
        }

    def _load(self):
        parser = configparser.ConfigParser(interpolation=None,
                                           inline_comment_prefixes=("#", ";"))
        parser.read(self.path, encoding="utf-8")
        for section, obj in self._sections().items():
            if not parser.has_section(section):
                continue
            for f in fields(obj):
                if parser.has_option(section, f.name):
                    try:
                        value = _convert(parser.get(section, f.name), type(getattr(obj, f.name)))
                        setattr(obj, f.name, value)
                    except ValueError:
                        print(f"[配置] {section}.{f.name} 值无效，使用默认值")

    def save_value(self, section: str, key: str, value):
        """只改写指定行，保留文件中的注释"""
        if not os.path.exists(self.path):
            return
        with open(self.path, "r", encoding="utf-8") as fp:
            lines = fp.readlines()
        current = None
        for i, line in enumerate(lines):
            m = re.match(r"\s*\[(.+?)\]", line)
            if m:
                current = m.group(1)
                continue
            if current == section and re.match(rf"\s*{re.escape(key)}\s*=", line):
                lines[i] = f"{key} = {value}\n"
                break
        with open(self.path, "w", encoding="utf-8") as fp:
            fp.writelines(lines)
