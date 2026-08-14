#pragma once
#include <string>
#include <Windows.h>

extern std::string g_logPath;
extern bool g_debugMode;

// Unified log rotation (replaces both RotateLogsW and RotateLogsA)
void RotateLogs(const std::wstring& basePath);

std::string GetLogPath();
void LogHost(const char* category, const std::string& msg);
std::string TruncateForLog(const std::wstring& ws, size_t maxChars = 60);
