#pragma once
#include <string>
#include <Windows.h>

extern std::string g_logPath;
extern bool        g_debugMode;

// Unified log rotation
void RotateLogs(const std::wstring& basePath);

// Returns resolved log file path (respects --log-path / --log-at-run-path)
std::string GetLogPath();

// Log an entry.
// level: maps to g_logLevel filter. Entries below g_logLevel are discarded.
// category: short tag shown in log output (e.g. "HOST", "WARN", "MONITOR")
// Default level = Info so all existing call sites work unchanged.
void LogHost(const char* category, const std::string& msg);
void LogHost(const char* category, const std::string& msg, int level); // level as int from LogLevel

// Convenience wrappers for explicit levels
void LogDebug(const char* category, const std::string& msg);
void LogWarn (const char* category, const std::string& msg);
void LogError(const char* category, const std::string& msg);
void LogFatal(const char* category, const std::string& msg);

std::string TruncateForLog(const std::wstring& ws, size_t maxChars = 60);
