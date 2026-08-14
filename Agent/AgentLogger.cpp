#include "pch.h"
#include "AgentLogger.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>

static std::mutex g_logMutex;

std::string GetLogPath() {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, path, MAX_PATH)) {
        std::wstring wPath(path);
        size_t pos = wPath.find_last_of(L"\\");
        if (pos != std::wstring::npos) {
            std::wstring dir = wPath.substr(0, pos); // Thư mục chứa dll
            if (dir.find(L"x64\\Release") != std::wstring::npos || dir.find(L"x64\\Debug") != std::wstring::npos) {
                pos = dir.find_last_of(L"\\");
                if (pos != std::wstring::npos) {
                    std::wstring root = dir.substr(0, pos);
                    pos = root.find_last_of(L"\\");
                    if (pos != std::wstring::npos) {
                        std::wstring projectRoot = root.substr(0, pos);
                        std::wstring logFile = projectRoot + L"\\logs\\mslc_agent_debug.log";
                        return std::string(logFile.begin(), logFile.end());
                    }
                }
            } else {
                std::wstring logFile = dir + L"\\logs\\mslc_agent_debug.log";
                return std::string(logFile.begin(), logFile.end());
            }
        }
    }
    return "C:\\Users\\Public\\mslc_agent_debug.log"; // Fallback
}

void LogToFile(const char* level, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    std::ostringstream entry;
    entry << '['
          << std::setfill('0')
          << std::setw(4) << st.wYear  << '-'
          << std::setw(2) << st.wMonth << '-'
          << std::setw(2) << st.wDay   << 'T'
          << std::setw(2) << st.wHour  << ':'
          << std::setw(2) << st.wMinute << ':'
          << std::setw(2) << st.wSecond
          << "] ["
          << level
          << "] [Agent] "
          << msg;

    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream logFile(g_logPath, std::ios_base::app);
    if (logFile.is_open()) {
        logFile << entry.str() << '\n';
    }
}
