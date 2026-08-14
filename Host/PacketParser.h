#pragma once
#include <string>
#include <Windows.h>

struct PipePacket {
    std::wstring text;
    bool         is_final = false;
    DWORD64      bytes    = 0;
    DWORD64      ts_ms    = 0;
    DWORD64      offset   = 0;
    DWORD64      duration = 0;
    std::wstring result_id;
};

inline bool ParsePacket(const std::wstring& data, PipePacket& out) {
    size_t p = data.find(L"\"text\":\"");
    if (p == std::wstring::npos) return false;
    p += 8;
    size_t e = data.find(L'"', p);
    if (e == std::wstring::npos) return false;
    out.text = data.substr(p, e - p);

    out.is_final = (data.find(L"\"is_final\":true") != std::wstring::npos);

    p = data.find(L"\"bytes\":");
    if (p != std::wstring::npos) {
        out.bytes = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 8));
    }

    p = data.find(L"\"ts_ms\":");
    if (p != std::wstring::npos) {
        out.ts_ms = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 8));
    }

    p = data.find(L"\"offset\":");
    if (p != std::wstring::npos) {
        out.offset = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 9));
    }

    p = data.find(L"\"duration\":");
    if (p != std::wstring::npos) {
        out.duration = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 11));
    }

    p = data.find(L"\"result_id\":\"");
    if (p != std::wstring::npos) {
        p += 13;
        size_t r_end = data.find(L'"', p);
        if (r_end != std::wstring::npos) {
            out.result_id = data.substr(p, r_end - p);
        }
    }
    return true;
}
