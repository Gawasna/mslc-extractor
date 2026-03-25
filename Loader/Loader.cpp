// Loader.cpp - Split-view console UI for Live Caption Extractor
// Layout: [LIVE STREAM] | [CONFIRMED SENTENCES] | [STATS]
#include <iostream>
#include <Windows.h>
#include <string>
#include <thread>
#include <TlHelp32.h>
#include <AclAPI.h>
#include <fcntl.h>
#include <io.h>
#include <vector>
#include <deque>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <fstream>


// =============================================================
// CONSTANTS
// =============================================================
static constexpr const wchar_t* PIPE_NAME   = L"\\\\.\\pipe\\LiveCaptionPipe";
static constexpr const wchar_t* TARGET_APP  = L"LiveCaptions.exe";
// Panel column config (characters). Adjust to taste.
static constexpr int COL_LIVE_W     = 46;   // Live stream panel width
static constexpr int COL_CONFIRM_W  = 46;   // Confirmed sentences panel width
static constexpr int COL_STATS_W    = 26;   // Stats panel width
static constexpr int CONSOLE_H      = 40;   // Desired console height (rows)
// Derived X origins (1-indexed: +1 for border char)
static constexpr int COL_LIVE_X     = 0;
static constexpr int COL_CONFIRM_X  = COL_LIVE_W + 1;           // after border
static constexpr int COL_STATS_X    = COL_CONFIRM_X + COL_CONFIRM_W + 1;
static constexpr int TOTAL_W        = COL_STATS_X + COL_STATS_W + 1;
// Content rows (below header row 0 and separator row 1)
static constexpr SHORT CONTENT_TOP  = 2;

// =============================================================
// LOADER DEBUG LOGGER
// Keeps a rolling window of the last LOG_MAX_LINES log entries.
// Each write flushes the full ring to disk (file stays small: <=100 lines).
// Uses a separate CRITICAL_SECTION from g_cs to prevent deadlock.
// Log file: C:\Users\Public\loader_debug.txt
// =============================================================
static constexpr const char* LOADER_LOG_PATH  = "C:\\Users\\Public\\loader_debug.txt";
static constexpr size_t      LOG_MAX_LINES     = 100;
static CRITICAL_SECTION      g_logCs;
static std::deque<std::string> g_logRing;

void LogLoader(const char* category, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    // Format: [HH:MM:SS.mmm] [CATEGORY] msg
    std::ostringstream entry;
    entry << '['
          << std::setfill('0')
          << std::setw(2) << st.wHour   << ':'
          << std::setw(2) << st.wMinute << ':'
          << std::setw(2) << st.wSecond << '.'
          << std::setw(3) << st.wMilliseconds
          << "] ["
          << std::left << std::setw(8) << category
          << "] "
          << msg;

    EnterCriticalSection(&g_logCs);
    g_logRing.push_back(entry.str());
    if (g_logRing.size() > LOG_MAX_LINES) g_logRing.pop_front();

    // Rewrite full ring to disk on every call.
    // File is at most ~8KB (100 lines x ~80 chars) - acceptable for debug.
    std::ofstream f(LOADER_LOG_PATH, std::ios_base::trunc);
    if (f.is_open()) {
        for (const auto& line : g_logRing) f << line << '\n';
    }
    LeaveCriticalSection(&g_logCs);
}

// Helper: narrow-string truncate for logging long text without bloating log
static std::string TruncateForLog(const std::wstring& ws, size_t maxChars = 60) {
    std::string narrow(ws.begin(), ws.end());
    if (narrow.size() > maxChars) {
        return narrow.substr(0, maxChars) + "...";
    }
    return narrow;
}


// =============================================================
// SPLIT-VIEW STATE (guarded by g_cs)
// =============================================================
static CRITICAL_SECTION g_cs;
static SHORT             g_liveRow     = CONTENT_TOP;
static SHORT             g_confirmRow  = CONTENT_TOP;
static DWORD64           g_pktCount    = 0;
static DWORD64           g_totalBytes  = 0;
static DWORD64           g_lastDelayMs = 0;
static wchar_t           g_lastTs[20]  = L"--:--:--";

// =============================================================
// CONSOLE HELPERS
// =============================================================

// Fix E0311: custom wrapper avoids name collision with WinAPI SetCursorPos
void MoveConsoleCursor(SHORT x, SHORT y) {
    COORD pos = { x, y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), pos);
}

// Clear one line within a panel (fill with spaces up to panel width)
void ClearPanelLine(SHORT x, SHORT y, int width) {
    MoveConsoleCursor(x, y);
    std::wstring blank(static_cast<size_t>(width), L' ');
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), blank.c_str(),
                  static_cast<DWORD>(blank.size()), &written, nullptr);
}

// Write text to a panel position, truncating at panel width
void WritePanelText(SHORT x, SHORT y, int panel_width, const std::wstring& text) {
    ClearPanelLine(x, y, panel_width);
    MoveConsoleCursor(x, y);
    std::wstring clipped = text.substr(0, static_cast<size_t>(panel_width));
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), clipped.c_str(),
                  static_cast<DWORD>(clipped.size()), &written, nullptr);
}

// Draw full frame (called once on startup)
void DrawFrame() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    MoveConsoleCursor(0, 0);

    // Header row
    auto padCenter = [](const std::wstring& s, int w) -> std::wstring {
        if (static_cast<int>(s.size()) >= w) return s.substr(0, w);
        int pad = (w - static_cast<int>(s.size())) / 2;
        return std::wstring(pad, L' ') + s + std::wstring(w - pad - static_cast<int>(s.size()), L' ');
    };
    std::wstring header =
        padCenter(L"LIVE STREAM",           COL_LIVE_W)    + L"\u2502" +
        padCenter(L"CONFIRMED SENTENCES",   COL_CONFIRM_W) + L"\u2502" +
        padCenter(L"STATS",                 COL_STATS_W);

    DWORD written = 0;
    WriteConsoleW(hOut, header.c_str(), static_cast<DWORD>(header.size()), &written, nullptr);

    // Separator row (box-drawing: horizontal line + cross joints)
    MoveConsoleCursor(0, 1);
    std::wstring sep;
    sep.reserve(TOTAL_W);
    for (int i = 0; i < TOTAL_W; ++i) {
        int c = i - COL_LIVE_W;
        if (c == 0 || c == COL_CONFIRM_W + 1)  sep += L'\u253C'; // cross +
        else                                     sep += L'\u2500'; // horizontal -
    }
    WriteConsoleW(hOut, sep.c_str(), static_cast<DWORD>(sep.size()), &written, nullptr);
}

// Redraw the vertical dividers on every content row (keeps borders visible after scrolling)
void RedrawDividers(SHORT y) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    MoveConsoleCursor(static_cast<SHORT>(COL_CONFIRM_X - 1), y);
    WriteConsoleW(hOut, L"\u2502", 1, &written, nullptr);
    MoveConsoleCursor(static_cast<SHORT>(COL_STATS_X - 1), y);
    WriteConsoleW(hOut, L"\u2502", 1, &written, nullptr);
}

// =============================================================
// STATS PANEL RENDERER (right column)
// =============================================================
void UpdateStatsPanel() {
    // Stats panel rows are fixed - overwrite in place
    const SHORT x = static_cast<SHORT>(COL_STATS_X);

    auto writeRow = [&](SHORT row, const std::wstring& label, const std::wstring& val) {
        std::wstring line = label + val;
        WritePanelText(x, row, COL_STATS_W, line);
        RedrawDividers(row);
    };

    writeRow(CONTENT_TOP + 0, L"Pkts  : ", std::to_wstring(g_pktCount));
    writeRow(CONTENT_TOP + 1, L"Bytes : ", std::to_wstring(g_totalBytes));
    const DWORD64 avg = (g_pktCount > 0) ? (g_totalBytes / g_pktCount) : 0;
    writeRow(CONTENT_TOP + 2, L"Avg   : ", std::to_wstring(avg) + L" B");
    writeRow(CONTENT_TOP + 3, L"Delay : ", std::to_wstring(g_lastDelayMs) + L" ms");
    writeRow(CONTENT_TOP + 4, L"Last  : ", std::wstring(g_lastTs));
}

// Convert ts_ms (GetTickCount64 from HookCore) to HH:MM:SS display string
static void FormatTimestamp(DWORD64 ts_ms, wchar_t* buf, size_t bufLen) {
    DWORD64 total_sec = ts_ms / 1000;
    DWORD64 h = total_sec / 3600;
    DWORD64 m = (total_sec % 3600) / 60;
    DWORD64 s = total_sec % 60;
    _snwprintf_s(buf, bufLen, bufLen - 1, L"%02llu:%02llu:%02llu", h, m, s);
}

// =============================================================
// JSON PARSER (manual, small payload only)
// =============================================================
struct PipePacket {
    std::wstring text;
    bool         is_final = false;
    DWORD64      bytes    = 0;
    DWORD64      ts_ms    = 0;
};

// Returns false if parse fails (packet malformed)
bool ParsePacket(const std::vector<wchar_t>& buf, size_t len, PipePacket& out) {
    std::wstring data(buf.data(), len);

    // text
    size_t p = data.find(L"\"text\":\"");
    if (p == std::wstring::npos) return false;
    p += 8;
    size_t e = data.find(L'"', p);
    if (e == std::wstring::npos) return false;
    out.text = data.substr(p, e - p);

    // is_final
    out.is_final = (data.find(L"\"is_final\":true") != std::wstring::npos);

    // bytes
    p = data.find(L"\"bytes\":");
    if (p != std::wstring::npos) {
        out.bytes = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 8));
    }

    // ts_ms
    p = data.find(L"\"ts_ms\":");
    if (p != std::wstring::npos) {
        out.ts_ms = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 8));
    }
    return true;
}

// =============================================================
// SENTENCE SPLITTER - Delta Watermark State Machine
//
// Problem: PARTIAL packets are CUMULATIVE (full utterance text, not delta).
//   Speaker speaks fast -> engine batches 3-4 sentences into one FINAL.
//   If we split by punctuation naively, FINAL re-delivers the same text -> duplicate.
//
// Solution: track `confirmed_len` = chars already committed to Confirmed panel.
//   Each packet: scan only [confirmed_len .. end] for sentence boundaries.
//   FINAL:        commit the remaining tail (even without punctuation), then reset.
//   Invariant:    the same character position is NEVER committed twice.
// =============================================================
struct SentenceSplitter {
    // Punctuation characters that mark end of a sentence segment
    static constexpr wchar_t BOUNDARIES[] = L".?!";

    std::wstring prev_text;     // Last partial text seen (to detect regression/reset)
    size_t       confirmed_len; // Watermark: chars already sent to Confirmed panel
    int          sentence_idx;  // Running sentence counter for labelling

    SentenceSplitter() : confirmed_len(0), sentence_idx(0) {}

    // Reset state at start of new utterance
    void Reset() {
        prev_text.clear();
        confirmed_len = 0;
        // sentence_idx is intentionally NOT reset - labels stay monotonically increasing
    }

    // Returns a list of new complete sentences extracted from `text` since last call.
    // Caller is responsible for rendering each entry to the Confirmed panel.
    std::vector<std::wstring> ExtractNewSentences(const std::wstring& text, bool is_final) {
        std::vector<std::wstring> results;

        // Guard: if engine restarted mid-stream (new text shorter than prev), reset
        if (text.size() < prev_text.size()) {
            LogLoader("SPLITTER",
                "REGRESSION detected: prev_len=" + std::to_string(prev_text.size()) +
                " new_len=" + std::to_string(text.size()) + " -> RESET watermark");
            Reset();
        }
        prev_text = text;

        if (is_final) {
            std::wstring tail = text.substr(confirmed_len);
            size_t start = tail.find_first_not_of(L' ');
            if (start != std::wstring::npos) tail = tail.substr(start);

            if (tail.empty()) {
                LogLoader("SPLITTER",
                    "FINAL arrived. tail=EMPTY (all sentences already committed). watermark="
                    + std::to_string(confirmed_len) + " -> RESET");
            } else {
                LogLoader("SPLITTER",
                    "FINAL arrived. tail_len=" + std::to_string(tail.size()) +
                    " watermark=" + std::to_string(confirmed_len) +
                    " tail=\"" + TruncateForLog(tail) + "\" -> COMMIT & RESET");
                results.push_back(tail);
            }
            Reset();
            return results;
        }

        // PARTIAL: scan for sentence boundaries ONLY in the unconfirmed suffix
        // We look for: [.?!] followed by a space OR end-of-string.
        // Comma is excluded here - commas mid-sentence are too aggressive to split on.
        size_t scan_pos   = confirmed_len;
        size_t commit_pos = confirmed_len;

        LogLoader("SPLITTER",
            "PARTIAL scan: text_len=" + std::to_string(text.size()) +
            " watermark=" + std::to_string(confirmed_len) +
            " scanning [" + std::to_string(confirmed_len) + ".." + std::to_string(text.size()) + "]");

        while (scan_pos < text.size()) {
            const wchar_t ch = text[scan_pos];
            const bool is_boundary = (ch == L'.' || ch == L'?' || ch == L'!');

            if (is_boundary) {
                const bool at_end            = (scan_pos + 1 >= text.size());
                const bool followed_by_space = !at_end && (text[scan_pos + 1] == L' ');

                if (at_end || followed_by_space) {
                    std::wstring sentence = text.substr(commit_pos, scan_pos - commit_pos + 1);
                    size_t trim = sentence.find_first_not_of(L' ');
                    if (trim != std::wstring::npos && trim > 0) sentence = sentence.substr(trim);

                    if (!sentence.empty()) {
                        LogLoader("EMIT",
                            "pos " + std::to_string(commit_pos) + ".." + std::to_string(scan_pos) +
                            " len=" + std::to_string(sentence.size()) +
                            " -> \"" + TruncateForLog(sentence) + "\"");
                        results.push_back(sentence);
                    }
                    commit_pos = scan_pos + 1;
                }
            }
            ++scan_pos;
        }

        const size_t old_watermark = confirmed_len;
        confirmed_len = commit_pos;

        LogLoader("SPLITTER",
            "PARTIAL done: emitted=" + std::to_string(results.size()) +
            " watermark " + std::to_string(old_watermark) +
            " -> " + std::to_string(confirmed_len));

        return results;
    }
};

// One splitter instance per utterance session (reset on pipe reconnect)
static SentenceSplitter g_splitter;


// =============================================================
// PIPE LISTENER + CONSOLE RENDER (runs on dedicated thread)
// =============================================================
void PipeListener() {
    // NULL DACL is required for AppContainer (sandbox) client to connect.
    // C6248 is suppressed intentionally - the design demands open access for IPC.
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
#pragma warning(suppress: 6248)
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };

    while (true) {
        HANDLE hPipe = CreateNamedPipeW(PIPE_NAME,
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, &sa);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        if (!ConnectNamedPipe(hPipe, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(hPipe);
            continue;
        }

        // Fix C6262: buffer on heap via vector instead of wchar_t buf[8192] on stack
        static constexpr DWORD PIPE_BUF_BYTES = 65536;
        std::vector<char>    rawBuf(PIPE_BUF_BYTES);
        std::vector<wchar_t> wideBuf(PIPE_BUF_BYTES / sizeof(char) + 1);
        DWORD bytesRead = 0;
        LogLoader("PIPE", "Client connected. Pipe session started.");


        while (ReadFile(hPipe, rawBuf.data(),
                        static_cast<DWORD>(rawBuf.size() - 1),
                        &bytesRead, NULL) && bytesRead > 0)
        {
            rawBuf[bytesRead] = '\0';
            const DWORD64 recvTick = GetTickCount64();

            // Convert narrow -> wide
            int wideLen = MultiByteToWideChar(CP_UTF8, 0, rawBuf.data(), -1,
                                              wideBuf.data(),
                                              static_cast<int>(wideBuf.size()));
            if (wideLen <= 0) {
                LogLoader("PIPE", "MultiByteToWideChar failed on raw packet. Skipping.");
                continue;
            }

            PipePacket pkt;
            const size_t charLen = static_cast<size_t>(wideLen - 1);
            if (!ParsePacket(wideBuf, charLen, pkt) || pkt.text.empty()) {
                LogLoader("PIPE", "ParsePacket failed or empty text. raw_bytes=" + std::to_string(bytesRead));
                continue;
            }

            // Both recvTick and pkt.ts_ms are GetTickCount64() from the same machine.
            // Direct subtraction is correct. Guard against tiny clock skew (recvTick < ts_ms).
            const DWORD64 delayMs = (pkt.ts_ms > 0 && recvTick >= pkt.ts_ms)
                ? (recvTick - pkt.ts_ms)
                : 0;


            // Log packet summary BEFORE acquiring g_cs (LogLoader uses g_logCs, no deadlock)
            LogLoader("PKT",
                std::string(pkt.is_final ? "FINAL  " : "PARTIAL") +
                " text_len=" + std::to_string(pkt.text.size()) +
                " bytes=" + std::to_string(pkt.bytes) +
                " delay=" + std::to_string(delayMs) + "ms" +
                " preview=\"" + TruncateForLog(pkt.text, 40) + "\"");

            EnterCriticalSection(&g_cs);

            // --- Update stats ---
            g_pktCount++;
            g_totalBytes  += pkt.bytes;
            g_lastDelayMs  = delayMs;
            FormatTimestamp(recvTick, g_lastTs, 20);

            // --- Render LIVE STREAM panel (left) ---
            const std::wstring livePrefix = pkt.is_final ? L"[F] " : L"[~] ";
            WritePanelText(static_cast<SHORT>(COL_LIVE_X), g_liveRow,
                           COL_LIVE_W, livePrefix + pkt.text);
            RedrawDividers(g_liveRow);
            g_liveRow++;
            if (g_liveRow >= CONSOLE_H - 1) g_liveRow = CONTENT_TOP;  // wrap

            // --- Render CONFIRMED panel (centre) via delta splitter ---
            {
                auto sentences = g_splitter.ExtractNewSentences(pkt.text, pkt.is_final);
                if (sentences.empty()) {
                    LogLoader("CONFIRM", "No new sentences emitted for this packet.");
                }
                for (const std::wstring& s : sentences) {
                    ++g_splitter.sentence_idx;
                    LogLoader("CONFIRM",
                        "#" + std::to_string(g_splitter.sentence_idx) +
                        " -> \"" + TruncateForLog(s) + "\"");
                    std::wstring label = std::to_wstring(g_splitter.sentence_idx) + L". ";
                    WritePanelText(static_cast<SHORT>(COL_CONFIRM_X), g_confirmRow,
                                   COL_CONFIRM_W, label + s);
                    RedrawDividers(g_confirmRow);
                    g_confirmRow++;
                    if (g_confirmRow >= CONSOLE_H - 1) g_confirmRow = CONTENT_TOP;
                }
            }

            // --- Refresh stats panel ---
            UpdateStatsPanel();

            LeaveCriticalSection(&g_cs);
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
        LogLoader("PIPE", "Client disconnected. Resetting splitter.");
        // Reset splitter state on pipe disconnect - next utterance is a clean slate
        EnterCriticalSection(&g_cs);
        g_splitter.Reset();
        LeaveCriticalSection(&g_cs);
    }
}

// =============================================================
// PERMISSION & INJECTION
// =============================================================
void SetAppContainerPermission(const std::wstring& filePath) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea = { 0 };
    if (GetNamedSecurityInfoW(filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, const_cast<LPWSTR>(L"ALL APPLICATION PACKAGES"),
                                     GENERIC_READ | GENERIC_EXECUTE,
                                     SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        if (SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW(const_cast<LPWSTR>(filePath.c_str()),
                                  SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                  NULL, NULL, pNewDACL, NULL);
            LocalFree(pNewDACL);
        }
        LocalFree(pSD);
    }
}

DWORD GetProcessIdByName(const wchar_t* processName) {
    DWORD pid = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = { sizeof(entry) };
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, processName) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return pid;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    SetAppContainerPermission(dllPath);

    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProcess) return false;

    const size_t allocSize = (dllPath.length() + 1) * sizeof(wchar_t);
    void* remoteMem = VirtualAllocEx(hProcess, nullptr, allocSize,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        CloseHandle(hProcess);
        return false;
    }

    WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), allocSize, nullptr);

    // Fix C6387: validate hKernel32 before dereferencing
    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    FARPROC pLoadLib = GetProcAddress(hKernel32, "LoadLibraryW");
    if (!pLoadLib) {
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                        reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLib),
                                        remoteMem, 0, nullptr);
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
        CloseHandle(hProcess);
        return true;
    }

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hProcess);
    return false;
}

// =============================================================
// MAIN ENTRY
// =============================================================
int main() {
    // Force UTF-16 output for wide-string console rendering
    _setmode(_fileno(stdout), _O_U16TEXT);

    InitializeCriticalSection(&g_cs);
    InitializeCriticalSection(&g_logCs);
    LogLoader("SESSION", "=== Loader started ===");

    // Size and configure the console window
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SMALL_RECT windowSize = { 0, 0,
                              static_cast<SHORT>(TOTAL_W - 1),
                              static_cast<SHORT>(CONSOLE_H - 1) };
    COORD bufferSize     = { static_cast<SHORT>(TOTAL_W),
                             static_cast<SHORT>(CONSOLE_H) };
    SetConsoleScreenBufferSize(hOut, bufferSize);
    SetConsoleWindowInfo(hOut, TRUE, &windowSize);

    // Hide cursor for cleaner rendering
    CONSOLE_CURSOR_INFO ci = { 1, FALSE };
    SetConsoleCursorInfo(hOut, &ci);

    DrawFrame();

    // Start pipe listener (dedicated thread, receives data from HookCore)
    std::thread(PipeListener).detach();

    // Locate or launch LiveCaptions.exe
    DWORD pid = GetProcessIdByName(TARGET_APP);
    if (pid == 0) {
        MoveConsoleCursor(0, CONSOLE_H - 1);
        std::wcout << L"[i] Opening Live Captions settings...";
        ShellExecuteW(NULL, L"open", L"ms-settings:privacy-livecaptions",
                      NULL, NULL, SW_SHOWNORMAL);
        while (pid == 0) {
            pid = GetProcessIdByName(TARGET_APP);
            Sleep(1000);
        }
    }

    // Resolve DLL path relative to this executable
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring strPath(exePath);
    std::wstring dllPath = strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\HookCore.dll";

    MoveConsoleCursor(0, CONSOLE_H - 1);
    if (InjectDLL(pid, dllPath)) {
        std::wcout << L"[+] HookCore.dll injected OK. Listening for captions...";
    } else {
        std::wcout << L"[-] Injection failed. DLL: " << dllPath;
    }

    // Keep loader alive; pipe listener thread does the actual work
    while (true) {
        Sleep(500);
    }

    DeleteCriticalSection(&g_cs);
    return 0;
}