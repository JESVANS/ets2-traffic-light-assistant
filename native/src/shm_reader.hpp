#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace tlhud {

class ShmReader {
public:
    explicit ShmReader(const wchar_t* name);
    ~ShmReader();

    ShmReader(const ShmReader&) = delete;
    ShmReader& operator=(const ShmReader&) = delete;

    bool IsOpen() const { return ptr_ != nullptr; }
    size_t Size() const { return size_; }

    // Throttled reconnect; safe to call every frame.
    bool EnsureOpen(uint64_t now_ms);
    void Close();

    template <typename T>
    bool ReadStruct(T& out) const {
        if (!ptr_ || size_ < sizeof(T)) return false;
        std::memcpy(&out, ptr_, sizeof(T));
        return true;
    }

private:
    std::wstring name_;
    HANDLE handle_ = nullptr;
    const void* ptr_ = nullptr;
    size_t size_ = 0;
    uint64_t last_try_ms_ = 0;
};

} // namespace tlhud
