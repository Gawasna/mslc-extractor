#pragma once
#include <string>
#include <Windows.h>
#include <atomic>

// Must match ProcessMonitor.h — forward-declared here so AppConfig
// stays self-contained and ProcessMonitor can include AppConfig safely.
enum class OnExitAction {
    Quit,     // exit with HostExitCode::ProcessExited (default)
    Reinject, // wait for LiveCaptions to reappear, then reinject
    Relaunch, // auto-launch new instance + reinject
};

// Runtime flags (set once at startup from CLI args)
extern std::atomic<DWORD> g_targetPid;
extern std::atomic<bool>  g_needReinjection;
extern std::wstring       g_customPipeName;
extern bool               g_debugMode;
extern std::string        g_customLogPath;
extern bool               g_stdoutOnly;
extern bool               g_noSpawn;
extern bool               g_injectOnly;

// Process lifecycle flags
extern bool               g_watchMode;    // --watch: enables dedicated process-watcher thread
extern bool               g_autoLaunch;   // --auto-launch: launch LiveCaptions if not found
extern OnExitAction       g_onExitAction; // --on-exit [quit|reinject|relaunch]

void ParseCliArgs(int argc, char* argv[]);
