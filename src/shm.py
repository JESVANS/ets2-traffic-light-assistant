"""
Windows 命名共享内存只读访问（只打开已存在的映射，不会自行创建）
"""
import ctypes
from ctypes import wintypes
from typing import Optional

_k32 = ctypes.WinDLL("kernel32", use_last_error=True)

FILE_MAP_READ = 0x0004

_k32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
_k32.OpenFileMappingW.restype = wintypes.HANDLE
_k32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD,
                               wintypes.DWORD, ctypes.c_size_t]
_k32.MapViewOfFile.restype = ctypes.c_void_p
_k32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
_k32.UnmapViewOfFile.restype = wintypes.BOOL
_k32.CloseHandle.argtypes = [wintypes.HANDLE]
_k32.CloseHandle.restype = wintypes.BOOL


class _MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wintypes.DWORD),
        ("PartitionId", wintypes.WORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wintypes.DWORD),
        ("Protect", wintypes.DWORD),
        ("Type", wintypes.DWORD),
    ]


_k32.VirtualQuery.argtypes = [ctypes.c_void_p, ctypes.POINTER(_MBI), ctypes.c_size_t]
_k32.VirtualQuery.restype = ctypes.c_size_t


class SharedMemoryReader:
    def __init__(self, name: str):
        self.name = name
        self._handle = None
        self._ptr = None
        self.size = 0

    @property
    def is_open(self) -> bool:
        return self._ptr is not None

    def open(self) -> bool:
        if self._ptr:
            return True
        handle = _k32.OpenFileMappingW(FILE_MAP_READ, False, self.name)
        if not handle:
            return False
        ptr = _k32.MapViewOfFile(handle, FILE_MAP_READ, 0, 0, 0)
        if not ptr:
            _k32.CloseHandle(handle)
            return False
        mbi = _MBI()
        _k32.VirtualQuery(ptr, ctypes.byref(mbi), ctypes.sizeof(mbi))
        self._handle, self._ptr, self.size = handle, ptr, int(mbi.RegionSize)
        return True

    def read(self, offset: int = 0, length: Optional[int] = None) -> bytes:
        if not self._ptr:
            raise RuntimeError(f"共享内存 {self.name} 未打开")
        if length is None:
            length = self.size - offset
        length = max(0, min(length, self.size - offset))
        return ctypes.string_at(self._ptr + offset, length)

    def close(self):
        if self._ptr:
            _k32.UnmapViewOfFile(self._ptr)
            self._ptr = None
        if self._handle:
            _k32.CloseHandle(self._handle)
            self._handle = None
        self.size = 0
