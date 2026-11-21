#include "shm_reader.hpp"

namespace tlhud {

ShmReader::ShmReader(const wchar_t* name) : name_(name ? name : L"") {}

ShmReader::~ShmReader() {
    Close();
}

bool ShmReader::EnsureOpen(uint64_t now_ms) {
    if (ptr_) return true;
    if (now_ms - last_try_ms_ < 500) return false;
    last_try_ms_ = now_ms;

    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name_.c_str());
    if (!h) return false;

    const void* p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0);
    if (!p) {
        CloseHandle(h);
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) {
        UnmapViewOfFile(p);
        CloseHandle(h);
        return false;
    }

    handle_ = h;
    ptr_ = p;
    size_ = (size_t)mbi.RegionSize;
    return true;
}

void ShmReader::Close() {
    if (ptr_) {
        UnmapViewOfFile(ptr_);
        ptr_ = nullptr;
    }
    if (handle_) {
        CloseHandle(handle_);
        handle_ = nullptr;
    }
    size_ = 0;
}

} // namespace tlhud
