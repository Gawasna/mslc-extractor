#include "AppConfig.h"
#include <string>
#include <algorithm>
#include <cctype>

// -- Injection / targeting
std::atomic<DWORD> g_targetPid{0};
std::atomic<bool>  g_needReinjection{false};
std::wstring       g_customPipeName = L"\\\\.\\pipe\\LiveCaptionPipe";
bool               g_noSpawn       = false;
bool               g_injectOnly    = false;

// -- Process lifecycle
bool               g_watchMode     = false;
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
bool               g_autoLaunch    = false;
#endif
OnExitAction       g_onExitAction  = OnExitAction::Quit;

// -- Logging
std::string        g_logPath           = "";
std::string        g_customLogPath     = "";
bool               g_logAtRunPath      = false;
bool               g_noLog             = false;
int                g_maxLogFiles       = 4;
long long          g_maxLogSizeBytes   = 0;  // 0 = unlimited
LogLevel           g_logLevel          = LogLevel::Info;

// -- Console / output
bool               g_debugMode    = false;
bool               g_stdoutOnly   = false;
bool               g_silent       = false;

// ---------------------------------------------------------------
LogLevel ParseLogLevel(const std::string& s) {
    std::string lo = s;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    if (lo == "debug") return LogLevel::Debug;
    if (lo == "info")  return LogLevel::Info;
    if (lo == "warn")  return LogLevel::Warn;
    if (lo == "error") return LogLevel::Error;
    if (lo == "fatal") return LogLevel::Fatal;
    if (lo == "none")  return LogLevel::None;
    return LogLevel::Info; // default for unknown
}

std::string LogLevelToString(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
        case LogLevel::None:  return "NONE ";
    }
    return "INFO ";
}

OnExitAction ParseOnExitAction(const std::string& s) {
    std::string lo = s;
    std::transform(lo.begin(), lo.end(), lo.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
    if (lo == "relaunch") return OnExitAction::Relaunch;
#endif
    if (lo == "reinject") return OnExitAction::Reinject;
    return OnExitAction::Quit;
}

// ---------------------------------------------------------------
void ParseCliArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        // -- Targeting
        if (arg == "-p" || arg == "--pid") {
            if (i + 1 < argc) {
                try { g_targetPid = static_cast<DWORD>(std::stoul(argv[++i])); }
                catch (const std::exception&) { /* invalid — stay 0 */ }
            }
        } else if (arg == "-n" || arg == "--pipe-name") {
            if (i + 1 < argc) {
                std::string ps = argv[++i];
                g_customPipeName = std::wstring(ps.begin(), ps.end());
            }
        } else if (arg == "--no-spawn") {
            g_noSpawn = true;
        } else if (arg == "--inject-only") {
            g_injectOnly = true;

        // -- Process lifecycle
        } else if (arg == "--watch") {
            g_watchMode = true;
#if 0 // DEPRECATED: AV evasion tactics. Auto-launch is now disabled.
        } else if (arg == "--auto-launch") {
            g_autoLaunch = true;
#endif
        } else if (arg == "--on-exit") {
            if (i + 1 < argc) g_onExitAction = ParseOnExitAction(argv[++i]);

        // -- Logging
        } else if (arg == "--log-path") {
            if (i + 1 < argc) g_customLogPath = argv[++i];
        } else if (arg == "--log-at-run-path") {
            g_logAtRunPath = true;
        } else if (arg == "--no-log") {
            g_noLog   = true;
            g_logLevel = LogLevel::None;
        } else if (arg == "--max-logs") {
            if (i + 1 < argc) {
                try { g_maxLogFiles = std::stoi(argv[++i]); }
                catch (const std::exception&) {}
                if (g_maxLogFiles < 1) g_maxLogFiles = 1;
            }
        } else if (arg == "--max-log-size") {
            if (i + 1 < argc) {
                try { g_maxLogSizeBytes = std::stoll(argv[++i]); }
                catch (const std::exception&) {}
            }
        } else if (arg == "--log-level") {
            if (i + 1 < argc) g_logLevel = ParseLogLevel(argv[++i]);

        // -- Console / output
        } else if (arg == "-d" || arg == "--debug") {
            g_debugMode = true;
            // --debug implies at least Info visible on stderr
            if (g_logLevel > LogLevel::Debug) g_logLevel = LogLevel::Debug;
        } else if (arg == "--stdout") {
            g_stdoutOnly = true;
        } else if (arg == "--silent") {
            g_silent = true;
        }
    }
}
