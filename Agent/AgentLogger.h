#pragma once
#include <string>
#include <Windows.h>

extern HMODULE g_hModule;
extern std::string g_logPath;

std::string GetLogPath();
void LogToFile(const char* level, const std::string& msg);
inline void LogInfo (const std::string& m) { LogToFile("INFO ", m); }
inline void LogWarn (const std::string& m) { LogToFile("WARN ", m); }
inline void LogError(const std::string& m) { LogToFile("ERROR", m); }
inline void LogFatal(const std::string& m) { LogToFile("FATAL", m); }
