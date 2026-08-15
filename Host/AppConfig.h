#pragma once
#include <string>
#include <Windows.h>
#include <atomic>

// ---------------------------------------------------------------
// Log level filter — controls which entries reach the log file.
// Higher = more restrictive. None = disable all logging.
// ---------------------------------------------------------------
enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
    Fatal = 4,
    None  = 5, // no file logging
};

LogLevel    ParseLogLevel(const std::string& s);
std::string LogLevelToString(LogLevel lv);

// ---------------------------------------------------------------
// --on-exit policy (also used by ProcessMonitor)
// ---------------------------------------------------------------
enum class OnExitAction {
    Quit,     // exit with HostExitCode::ProcessExited (default)
    Reinject, // wait for process to reappear by name, then reinject
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
    Relaunch, // auto-launch a new instance + reinject
#endif
};

OnExitAction ParseOnExitAction(const std::string& s);

// ---------------------------------------------------------------
// Runtime flags — set once at startup from CLI args
// ---------------------------------------------------------------

// -- Injection / targeting
extern std::atomic<DWORD> g_targetPid;
extern std::atomic<bool>  g_needReinjection;
extern std::wstring       g_customPipeName;
extern bool               g_noSpawn;
extern bool               g_injectOnly;

// -- Process lifecycle (new)
extern bool               g_watchMode;
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
extern bool               g_autoLaunch;
#endif
extern OnExitAction       g_onExitAction;

// -- Logging
extern std::string        g_logPath;       // resolved at startup by GetLogPath()
extern std::string        g_customLogPath; // --log-path <path>
extern bool               g_logAtRunPath;  // --log-at-run-path  (log next to exe, no logs/ subdir)
extern bool               g_noLog;         // --no-log           (disable file logging)
extern int                g_maxLogFiles;   // --max-logs N       (default 4)
extern long long          g_maxLogSizeBytes; // --max-log-size N (bytes; 0 = unlimited)
extern LogLevel           g_logLevel;      // --log-level [DEBUG|INFO|WARN|ERROR|FATAL|NONE]

// -- Console / output
extern bool               g_debugMode;    // --debug  (print log entries to stderr)
extern bool               g_stdoutOnly;   // --stdout (clean JSON to stdout)
extern bool               g_silent;       // --silent (no console output at all)

void ParseCliArgs(int argc, char* argv[]);
