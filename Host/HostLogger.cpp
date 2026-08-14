#include "HostLogger.h"
#include "AppConfig.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <deque>
#include <vector>
#include <algorithm>
#include <iostream>

static constexpr size_t        LOG_MAX_LINES = 100;
static std::mutex              g_logMutex;
static std::deque<std::string> g_logRing;

void RotateLogs(const std::wstring& basePath) {
    if (basePath.empty()) return;

    size_t lastSlash = basePath.find_last_of(L"\\/");
    std::wstring dir = (lastSlash != std::wstring::npos) ? basePath.substr(0, lastSlash + 1) : L"";
    std::wstring fileWithExt = (lastSlash != std::wstring::npos) ? basePath.substr(lastSlash + 1) : basePath;
    
    size_t lastDot = fileWithExt.find_last_of(L'.');
    std::wstring filename = (lastDot != std::wstring::npos) ? fileWithExt.substr(0, lastDot) : fileWithExt;
    std::wstring ext = (lastDot != std::wstring::npos) ? fileWithExt.substr(lastDot) : L"";

    DWORD attr = GetFileAttributesW(basePath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t ts[32];
        swprintf_s(ts, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        
        std::wstring archivePath = dir + filename + L"_" + ts + ext;
        MoveFileExW(basePath.c_str(), archivePath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    std::wstring searchPattern = dir + filename + L"_*_*" + ext;
    
    std::vector<std::wstring> archives;
    WIN32_FIND_DATAW findData;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &findData);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                archives.push_back(dir + findData.cFileName);
            }
        } while (FindNextFileW(hFind, &findData));
        FindClose(hFind);
    }

    std::sort(archives.begin(), archives.end());

    if (archives.size() > 4) {
        size_t filesToDelete = archives.size() - 4;
        for (size_t i = 0; i < filesToDelete; ++i) {
            DeleteFileW(archives[i].c_str());
        }
    }
}

void LogHost(const char* category, const std::string& msg) {
    if (g_debugMode) {
        std::cerr << "[" << category << "] " << msg << std::endl;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    DWORD threadId = GetCurrentThreadId();

    std::ostringstream entry;
    entry << '['
          << std::setfill('0')
          << std::setw(4) << st.wYear   << '-'
          << std::setw(2) << st.wMonth  << '-'
          << std::setw(2) << st.wDay    << 'T'
          << std::setw(2) << st.wHour   << ':'
          << std::setw(2) << st.wMinute << ':'
          << std::setw(2) << st.wSecond << '.'
          << std::setw(3) << st.wMilliseconds
          << "] [TID:"
          << std::setw(5) << threadId
          << "] ["
          << std::setfill(' ') // Reset fill character to space
          << std::left << std::setw(8) << category
          << "] "
          << msg;

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logRing.push_back(entry.str());
    if (g_logRing.size() > LOG_MAX_LINES) g_logRing.pop_front();
    
    std::ofstream f(g_logPath, std::ios_base::app);
    if (!f.is_open()) {
        g_logPath = "C:\\Users\\Public\\mslc_host_debug.log";
        f.open(g_logPath, std::ios_base::app);
    }
    if (f.is_open()) {
        f << entry.str() << '\n';
    }
}

std::string TruncateForLog(const std::wstring& ws, size_t maxChars) {
    std::string narrow;
    narrow.reserve(ws.size());
    for (wchar_t wc : ws) {
        narrow.push_back(static_cast<char>(wc));
    }
    if (narrow.size() > maxChars) {
        return narrow.substr(0, maxChars) + "...";
    }
    return narrow;
}

std::string GetLogPath() {
    if (!g_customLogPath.empty()) {
        return g_customLogPath;
    }

    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
        std::wstring wPath(path);
        size_t pos = wPath.find_last_of(L"\\");
        if (pos != std::wstring::npos) {
            std::wstring dir = wPath.substr(0, pos); // Thư mục chứa exe
            if (dir.find(L"x64\\Release") != std::wstring::npos || dir.find(L"x64\\Debug") != std::wstring::npos) {
                pos = dir.find_last_of(L"\\");
                if (pos != std::wstring::npos) {
                    std::wstring root = dir.substr(0, pos);
                    pos = root.find_last_of(L"\\");
                    if (pos != std::wstring::npos) {
                        std::wstring projectRoot = root.substr(0, pos);
                        std::wstring logFile = projectRoot + L"\\logs\\mslc_host_debug.log";
                        return std::string(logFile.begin(), logFile.end());
                    }
                }
            } else {
                std::wstring logFile = dir + L"\\logs\\mslc_host_debug.log";
                return std::string(logFile.begin(), logFile.end());
            }
        }
    }
    return "C:\\Users\\Public\\mslc_host_debug.log"; // Fallback
}
