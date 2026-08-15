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

static constexpr size_t        LOG_RING_LINES = 100;
static std::mutex              g_logMutex;
static std::deque<std::string> g_logRing;

// ---------------------------------------------------------------
// Map category string to numeric log level for filter comparison.
// Mirrors LogLevel enum values in AppConfig.h.
// ---------------------------------------------------------------
static int CategoryToLevel(const char* category) {
    if (!category) return 1; // Info
    std::string c = category;
    if (c.find("DEBUG") != std::string::npos) return 0;
    if (c.find("WARN")  != std::string::npos) return 2;
    if (c.find("ERROR") != std::string::npos) return 3;
    if (c.find("FATAL") != std::string::npos) return 4;
    return 1; // Info default
}

// ---------------------------------------------------------------
void RotateLogs(const std::wstring& basePath) {
    if (basePath.empty()) return;

    size_t lastSlash     = basePath.find_last_of(L"\\/");
    std::wstring dir     = (lastSlash != std::wstring::npos) ? basePath.substr(0, lastSlash + 1) : L"";
    std::wstring fileExt = (lastSlash != std::wstring::npos) ? basePath.substr(lastSlash + 1) : basePath;

    size_t lastDot        = fileExt.find_last_of(L'.');
    std::wstring filename = (lastDot != std::wstring::npos) ? fileExt.substr(0, lastDot) : fileExt;
    std::wstring ext      = (lastDot != std::wstring::npos) ? fileExt.substr(lastDot)    : L"";

    // Rename current log with timestamp
    DWORD attr = GetFileAttributesW(basePath.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t ts[32];
        swprintf_s(ts, L"%04d%02d%02d_%02d%02d%02d",
                   st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond);
        std::wstring archivePath = dir + filename + L"_" + ts + ext;
        MoveFileExW(basePath.c_str(), archivePath.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    // Prune oldest archives — respect g_maxLogFiles
    std::wstring searchPattern = dir + filename + L"_*" + ext;
    std::vector<std::wstring> archives;
    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                archives.push_back(dir + fd.cFileName);
        } while (FindNextFileW(hFind, &fd));
        FindClose(hFind);
    }

    std::sort(archives.begin(), archives.end());

    // g_maxLogFiles <= 0 → keep unlimited; otherwise prune oldest
    int keepCount = (g_maxLogFiles > 0) ? g_maxLogFiles : 8;
    if (static_cast<int>(archives.size()) > keepCount) {
        size_t toDelete = archives.size() - static_cast<size_t>(keepCount);
        for (size_t i = 0; i < toDelete; ++i)
            DeleteFileW(archives[i].c_str());
    }
}

// ---------------------------------------------------------------
// Internal: check if current log file exceeds size limit.
// If so, rotate now.
// ---------------------------------------------------------------
static void CheckSizeAndRotate() {
    if (g_maxLogSizeBytes <= 0 || g_logPath.empty()) return;

    WIN32_FILE_ATTRIBUTE_DATA info;
    std::wstring wPath(g_logPath.begin(), g_logPath.end());
    if (!GetFileAttributesExW(wPath.c_str(), GetFileExInfoStandard, &info)) return;

    long long fileSize = (static_cast<long long>(info.nFileSizeHigh) << 32)
                       | static_cast<long long>(info.nFileSizeLow);

    if (fileSize >= g_maxLogSizeBytes) {
        RotateLogs(wPath);
    }
}

// ---------------------------------------------------------------
void LogHost(const char* category, const std::string& msg, int level) {
    // --- Level filter ---
    int minLevel = static_cast<int>(g_logLevel);
    if (minLevel >= static_cast<int>(LogLevel::None)) return; // g_noLog path
    if (level < minLevel) return;

    // --- Build entry string ---
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD tid = GetCurrentThreadId();

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
          << "] [TID:" << std::setw(5) << tid
          << "] [" << std::setfill(' ') << std::left << std::setw(8) << category
          << "] " << msg;

    // --- Console output ---
    if (g_debugMode && !g_silent) {
        std::cerr << entry.str() << '\n';
    }

    // --- Ring buffer (in-memory) ---
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logRing.push_back(entry.str());
        if (g_logRing.size() > LOG_RING_LINES) g_logRing.pop_front();
    }

    // --- File output ---
    if (g_noLog || g_logPath.empty()) return;

    std::lock_guard<std::mutex> lock(g_logMutex);

    CheckSizeAndRotate(); // rotate if size limit exceeded

    std::ofstream f(g_logPath, std::ios_base::app);
    if (!f.is_open()) {
        // Fallback to public path
        g_logPath = "C:\\Users\\Public\\mslc_host_debug.log";
        f.open(g_logPath, std::ios_base::app);
    }
    if (f.is_open()) f << entry.str() << '\n';
}

// Default level overload — infers level from category string
void LogHost(const char* category, const std::string& msg) {
    LogHost(category, msg, CategoryToLevel(category));
}

// Convenience wrappers
void LogDebug(const char* category, const std::string& msg) {
    LogHost(category, msg, static_cast<int>(LogLevel::Debug));
}
void LogWarn(const char* category, const std::string& msg) {
    LogHost(category, msg, static_cast<int>(LogLevel::Warn));
}
void LogError(const char* category, const std::string& msg) {
    LogHost(category, msg, static_cast<int>(LogLevel::Error));
}
void LogFatal(const char* category, const std::string& msg) {
    LogHost(category, msg, static_cast<int>(LogLevel::Fatal));
}

// ---------------------------------------------------------------
std::string TruncateForLog(const std::wstring& ws, size_t maxChars) {
    std::string narrow;
    narrow.reserve(ws.size());
    for (wchar_t wc : ws) narrow.push_back(static_cast<char>(wc));
    if (narrow.size() > maxChars) return narrow.substr(0, maxChars) + "...";
    return narrow;
}

// ---------------------------------------------------------------
std::string GetLogPath() {
    // 1. Explicit --log-path wins
    if (!g_customLogPath.empty()) return g_customLogPath;

    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) {
        return "C:\\Users\\Public\\mslc_host_debug.log";
    }

    std::wstring wPath(path);
    size_t pos = wPath.find_last_of(L"\\");
    if (pos == std::wstring::npos) return "C:\\Users\\Public\\mslc_host_debug.log";

    std::wstring exeDir = wPath.substr(0, pos);

    // 2. --log-at-run-path: log directly next to exe (no logs/ subdir)
    if (g_logAtRunPath) {
        std::wstring logFile = exeDir + L"\\mslc_host_debug.log";
        return std::string(logFile.begin(), logFile.end());
    }

    // 3. Default: logs/ subfolder (walk up from x64/Release or x64/Debug)
    bool isBuiltDir = exeDir.find(L"x64\\Release") != std::wstring::npos
                   || exeDir.find(L"x64\\Debug")   != std::wstring::npos;

    std::wstring logDir;
    if (isBuiltDir) {
        // Walk up two levels: x64/Release -> x64 -> project root
        size_t p1 = exeDir.find_last_of(L"\\");
        if (p1 != std::wstring::npos) {
            size_t p2 = exeDir.find_last_of(L"\\", p1 - 1);
            if (p2 != std::wstring::npos) {
                logDir = exeDir.substr(0, p2) + L"\\logs";
            }
        }
    }
    if (logDir.empty()) logDir = exeDir + L"\\logs";

    std::wstring logFile = logDir + L"\\mslc_host_debug.log";
    return std::string(logFile.begin(), logFile.end());
}
