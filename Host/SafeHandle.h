#pragma once
#include <Windows.h>

class SafeHandle {
    HANDLE m_handle;
public:
    explicit SafeHandle(HANDLE h = INVALID_HANDLE_VALUE) : m_handle(h) {}
    ~SafeHandle() { Close(); }
    void Close() {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != NULL) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }
    HANDLE Get() const { return m_handle; }
    void Set(HANDLE h) { Close(); m_handle = h; }
    bool IsValid() const { return m_handle != INVALID_HANDLE_VALUE && m_handle != NULL; }
    HANDLE* AddrOf() { return &m_handle; }
    SafeHandle(const SafeHandle&) = delete;
    SafeHandle& operator=(const SafeHandle&) = delete;
    SafeHandle(SafeHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID_HANDLE_VALUE; }
    SafeHandle& operator=(SafeHandle&& other) noexcept {
        if (this != &other) { Close(); m_handle = other.m_handle; other.m_handle = INVALID_HANDLE_VALUE; }
        return *this;
    }
};
