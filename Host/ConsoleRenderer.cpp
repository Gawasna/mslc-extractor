#include "ConsoleRenderer.h"
#include <iostream>
#include <Windows.h>

static size_t g_lastLiveWidth = 0;

std::string WideToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

void PrintLiveText(const std::wstring& text) {
    int consoleWidth = 80;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        consoleWidth = csbi.dwSize.X;
    }
    if (consoleWidth <= 0) consoleWidth = 80;

    std::wstring prefix = L"\r[LIVE] [~] ";
    std::wstring displayOpts = text;

    if (prefix.size() + displayOpts.size() >= static_cast<size_t>(consoleWidth)) {
        size_t maxTextLen = consoleWidth - prefix.size() - 4;
        if (displayOpts.size() > maxTextLen) {
            displayOpts = L"..." + displayOpts.substr(displayOpts.size() - maxTextLen);
        }
    }

    std::wstring line = prefix + displayOpts;
    if (line.size() < g_lastLiveWidth) {
        line += std::wstring(g_lastLiveWidth - line.size(), L' ');
    }
    g_lastLiveWidth = line.size();
    std::cout << WideToUTF8(line);
    std::cout.flush();
}

void ClearLiveText() {
    if (g_lastLiveWidth > 0) {
        std::wstring clearLine(g_lastLiveWidth, L' ');
        std::cout << "\r" << WideToUTF8(clearLine) << "\r";
        std::cout.flush();
        g_lastLiveWidth = 0;
    }
}

void FormatTimestamp(DWORD64 ts_ms, wchar_t* buf, size_t bufLen) {
    DWORD64 total_sec = ts_ms / 1000;
    DWORD64 h = total_sec / 3600;
    DWORD64 m = (total_sec % 3600) / 60;
    DWORD64 s = total_sec % 60;
    _snwprintf_s(buf, bufLen, bufLen - 1, L"%02llu:%02llu:%02llu", h, m, s);
}
