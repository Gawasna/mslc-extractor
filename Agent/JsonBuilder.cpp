#include "pch.h"
#include "JsonBuilder.h"
#include <sstream>
#include <iomanip>
#include <cstring>

std::string BuildJsonPayload(const char* text, bool is_final, DWORD64 ts_ms, uint64_t offset, uint64_t duration, const char* result_id) {
    const size_t text_bytes = strlen(text);

    std::string escaped;
    escaped.reserve(text_bytes);
    for (const char* p = text; *p; ++p) {
        if (*p == '"')  escaped += "\\\"";
        else if (*p == '\\') escaped += "\\\\";
        else            escaped += *p;
    }

    std::ostringstream json;
    json << "{\"text\":\""   << escaped
         << "\",\"is_final\":"  << (is_final ? "true" : "false")
         << ",\"bytes\":"    << text_bytes
         << ",\"ts_ms\":"    << ts_ms
         << ",\"offset\":"   << offset
         << ",\"duration\":" << duration
         << ",\"result_id\":\"" << result_id << "\""
         << "}\n";
    return json.str();
}

std::string BuildEventJsonPayload(const std::string& eventName, const std::string& k1, const std::string& v1, const std::string& k2, const std::string& v2) {
    long long ticks = 0;
    GetSystemTimePreciseAsFileTime(reinterpret_cast<FILETIME*>(&ticks));
    std::ostringstream json;
    json << "{\"event\":\"" << eventName << "\",\"precise_ticks\":" << ticks << ",\"ts_ms\":" << GetTickCount64() << ",\"" << k1 << "\":\"" << v1 << "\",\"" << k2 << "\":\"" << v2 << "\"}\n";
    return json.str();
}

std::string HandleToHexString(void* handle) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(16) << std::setfill('0') << reinterpret_cast<uint64_t>(handle);
    return ss.str();
}
