#include "AppConfig.h"
#include <string>

std::atomic<DWORD> g_targetPid{0};
std::atomic<bool>  g_needReinjection{false};
std::wstring       g_customPipeName = L"\\\\.\\pipe\\LiveCaptionPipe";
bool               g_debugMode = false;
std::string        g_customLogPath = "";
bool               g_stdoutOnly = false;
bool               g_noSpawn = false;
bool               g_injectOnly = false;
std::string        g_logPath = "";

void ParseCliArgs(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" || arg == "--pid") {
            if (i + 1 < argc) {
                try {
                    g_targetPid = static_cast<DWORD>(std::stoul(argv[++i]));
                } catch (const std::exception&) {
                    // Invalid PID — ignore, stays 0 (auto-discovery)
                }
            }
        } else if (arg == "-n" || arg == "--pipe-name") {
            if (i + 1 < argc) {
                std::string pipeStr = argv[++i];
                g_customPipeName = std::wstring(pipeStr.begin(), pipeStr.end());
            }
        } else if (arg == "-d" || arg == "--debug") {
            g_debugMode = true;
        } else if (arg == "--log-path") {
            if (i + 1 < argc) {
                g_customLogPath = argv[++i];
            }
        } else if (arg == "--stdout") {
            g_stdoutOnly = true;
        } else if (arg == "--no-spawn") {
            g_noSpawn = true;
        } else if (arg == "--inject-only") {
            g_injectOnly = true;
        }
    }
}
