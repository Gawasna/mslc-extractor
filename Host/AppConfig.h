#pragma once
#include <string>
#include <Windows.h>
#include <atomic>

// Runtime flags (set once at startup)
extern std::atomic<DWORD> g_targetPid;
extern std::atomic<bool>  g_needReinjection;
extern std::wstring       g_customPipeName;
extern bool               g_debugMode;
extern std::string        g_customLogPath;
extern bool               g_stdoutOnly;
extern bool               g_noSpawn;
extern bool               g_injectOnly;

void ParseCliArgs(int argc, char* argv[]);
