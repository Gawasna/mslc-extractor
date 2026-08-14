#pragma once
#include <string>
#include <Windows.h>

std::string BuildJsonPayload(const char* text, bool is_final, DWORD64 ts_ms, uint64_t offset, uint64_t duration, const char* result_id);
std::string BuildEventJsonPayload(const std::string& eventName, const std::string& k1, const std::string& v1, const std::string& k2, const std::string& v2);
std::string HandleToHexString(void* handle);
