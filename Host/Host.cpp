#include <iostream>
#include <Windows.h>
#include <string>
#include <thread>
#include <TlHelp32.h>
#include <AclAPI.h>
#include <sddl.h>
#include <fcntl.h>
#include <io.h>
#include <vector>
#include <deque>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <memory>

#include <shlobj.h>
#pragma comment(lib, "shell32.lib")

// =============================================================
// CONSTANTS
// =============================================================
static constexpr const wchar_t* PIPE_NAME   = L"\\\\.\\pipe\\LiveCaptionPipe";
static constexpr const wchar_t* TARGET_APP  = L"LiveCaptions.exe";

// Helper to get AppData path for logging
std::string GetLogPath() {
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path))) {
        std::string fullPath = path;
        fullPath += "\\mslc_host_debug.txt";
        return fullPath;
    }
    return "C:\\Users\\Public\\mslc_host_debug.txt"; // Fallback
}

// Panel column config (characters).
static constexpr int COL_LIVE_W     = 46;   // Live stream panel width
static constexpr int COL_CONFIRM_W  = 46;   // Confirmed sentences panel width
static constexpr int COL_STATS_W    = 26;   // Stats panel width
static constexpr int CONSOLE_H      = 40;   // Desired console height (rows)

static constexpr int COL_LIVE_X     = 0;
static constexpr int COL_CONFIRM_X  = COL_LIVE_W + 1;
static constexpr int COL_STATS_X    = COL_CONFIRM_X + COL_CONFIRM_W + 1;
static constexpr int TOTAL_W        = COL_STATS_X + COL_STATS_W + 1;
static constexpr SHORT CONTENT_TOP  = 2;

// =============================================================
// RAII WRAPPER FOR WINDOWS API HANDLES
// =============================================================
class SafeHandle {
    HANDLE m_handle;
public:
    explicit SafeHandle(HANDLE h = INVALID_HANDLE_VALUE) : m_handle(h) {}
    ~SafeHandle() { Close(); }

    void Close() {
        if (m_handle != INVALID_HANDLE_VALUE && m_handle != NULL) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

    HANDLE Get() const { return m_handle; }
    void Set(HANDLE h) { Close(); m_handle = h; }
    bool IsValid() const { return m_handle != INVALID_HANDLE_VALUE && m_handle != NULL; }
    HANDLE* AddrOf() { return &m_handle; }

    SafeHandle(const SafeHandle&) = delete;
    SafeHandle& operator=(const SafeHandle&) = delete;
    SafeHandle(SafeHandle&& other) noexcept : m_handle(other.m_handle) { other.m_handle = INVALID_HANDLE_VALUE; }
    SafeHandle& operator=(SafeHandle&& other) noexcept {
        if (this != &other) {
            Close();
            m_handle = other.m_handle;
            other.m_handle = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};

// =============================================================
// HOST DEBUG LOGGER (Thread-Safe)
// =============================================================
static const std::string       HOST_LOG_PATH = GetLogPath();
static constexpr size_t        LOG_MAX_LINES = 100;
static std::mutex              g_logMutex;
static std::deque<std::string> g_logRing;

void LogLoader(const char* category, const std::string& msg) {
    SYSTEMTIME st;
    GetLocalTime(&st);

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

    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logRing.push_back(entry.str());
    if (g_logRing.size() > LOG_MAX_LINES) g_logRing.pop_front();

    std::ofstream f(HOST_LOG_PATH, std::ios_base::trunc);
    if (f.is_open()) {
        for (const auto& line : g_logRing) f << line << '\n';
    }
}

static std::string TruncateForLog(const std::wstring& ws, size_t maxChars = 60) {
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

// =============================================================
// SPLIT-VIEW STATE (guarded by g_csMutex)
// =============================================================
static std::mutex g_csMutex;
static SHORT      g_liveRow     = CONTENT_TOP;
static SHORT      g_confirmRow  = CONTENT_TOP;
static DWORD64    g_pktCount    = 0;
static DWORD64    g_totalBytes  = 0;
static DWORD64    g_lastDelayMs = 0;
static wchar_t    g_lastTs[20]  = L"--:--:--";

// =============================================================
// CONSOLE HELPERS
// =============================================================
void MoveConsoleCursor(SHORT x, SHORT y) {
    COORD pos = { x, y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), pos);
}

void ClearPanelLine(SHORT x, SHORT y, int width) {
    MoveConsoleCursor(x, y);
    std::wstring blank(static_cast<size_t>(width), L' ');
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), blank.c_str(),
                  static_cast<DWORD>(blank.size()), &written, nullptr);
}

void WritePanelText(SHORT x, SHORT y, int panel_width, const std::wstring& text) {
    ClearPanelLine(x, y, panel_width);
    MoveConsoleCursor(x, y);
    std::wstring clipped = text.substr(0, static_cast<size_t>(panel_width));
    DWORD written = 0;
    WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), clipped.c_str(),
                  static_cast<DWORD>(clipped.size()), &written, nullptr);
}

void DrawFrame() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    MoveConsoleCursor(0, 0);

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

    MoveConsoleCursor(0, 1);
    std::wstring sep;
    sep.reserve(TOTAL_W);
    for (int i = 0; i < TOTAL_W; ++i) {
        int c = i - COL_LIVE_W;
        if (c == 0 || c == COL_CONFIRM_W + 1)  sep += L'\u253C';
        else                                   sep += L'\u2500';
    }
    WriteConsoleW(hOut, sep.c_str(), static_cast<DWORD>(sep.size()), &written, nullptr);
}

void RedrawDividers(SHORT y) {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    MoveConsoleCursor(static_cast<SHORT>(COL_CONFIRM_X - 1), y);
    WriteConsoleW(hOut, L"\u2502", 1, &written, nullptr);
    MoveConsoleCursor(static_cast<SHORT>(COL_STATS_X - 1), y);
    WriteConsoleW(hOut, L"\u2502", 1, &written, nullptr);
}

void UpdateStatsPanel() {
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

static void FormatTimestamp(DWORD64 ts_ms, wchar_t* buf, size_t bufLen) {
    DWORD64 total_sec = ts_ms / 1000;
    DWORD64 h = total_sec / 3600;
    DWORD64 m = (total_sec % 3600) / 60;
    DWORD64 s = total_sec % 60;
    _snwprintf_s(buf, bufLen, bufLen - 1, L"%02llu:%02llu:%02llu", h, m, s);
}

// =============================================================
// JSON PARSER
// =============================================================
struct PipePacket {
    std::wstring text;
    bool         is_final = false;
    DWORD64      bytes    = 0;
    DWORD64      ts_ms    = 0;
};

bool ParsePacket(const std::wstring& data, PipePacket& out) {
    size_t p = data.find(L"\"text\":\"");
    if (p == std::wstring::npos) return false;
    p += 8;
    size_t e = data.find(L'"', p);
    if (e == std::wstring::npos) return false;
    out.text = data.substr(p, e - p);

    out.is_final = (data.find(L"\"is_final\":true") != std::wstring::npos);

    p = data.find(L"\"bytes\":");
    if (p != std::wstring::npos) {
        out.bytes = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 8));
    }

    p = data.find(L"\"ts_ms\":");
    if (p != std::wstring::npos) {
        out.ts_ms = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 8));
    }
    return true;
}

// =============================================================
// SENTENCE SPLITTER - Delta Watermark State Machine
// =============================================================
struct SentenceSplitter {
    static constexpr wchar_t BOUNDARIES[] = L".?!";

    std::wstring prev_text;
    size_t       confirmed_len;
    int          sentence_idx;

    SentenceSplitter() : confirmed_len(0), sentence_idx(0) {}

    void Reset() {
        prev_text.clear();
        confirmed_len = 0;
    }

    std::vector<std::wstring> ExtractNewSentences(const std::wstring& text, bool is_final) {
        std::vector<std::wstring> results;

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

            if (!tail.empty()) {
                LogLoader("SPLITTER",
                    "FINAL tail_len=" + std::to_string(tail.size()) +
                    " tail=\"" + TruncateForLog(tail) + "\" -> COMMIT & RESET");
                results.push_back(tail);
            }
            Reset();
            return results;
        }

        size_t scan_pos   = confirmed_len;
        size_t commit_pos = confirmed_len;

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
                        LogLoader("EMIT", "Emitting sentence: \"" + TruncateForLog(sentence) + "\"");
                        results.push_back(sentence);
                    }
                    commit_pos = scan_pos + 1;
                }
            }
            ++scan_pos;
        }

        confirmed_len = commit_pos;
        return results;
    }
};

static SentenceSplitter g_splitter;

// =============================================================
// DECOUPLED ARCHITECTURE: IPC PACKET QUEUE
// =============================================================
struct RawPacket {
    std::string data;
    DWORD64     recvTick;
};

static std::deque<RawPacket>    g_packetQueue;
static std::mutex               g_queueMutex;
static std::condition_variable  g_queueCv;
static std::atomic<bool>        g_exitHost{false};

// =============================================================
// ASYNCHRONOUS OVERLAPPED Named Pipe Listener Thread
// =============================================================
void PipeListener() {
    PSECURITY_DESCRIPTOR pSD = NULL;
    // Secure DACL: 
    // S-1-15-2-1: ALL APPLICATION PACKAGES (AppContainers) - Generic Read/Write
    // CO: Creator Owner - Generic All
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;S-1-15-2-1)(A;;GA;;;CO)", 
            SDDL_REVISION_1, 
            &pSD, 
            NULL)) {
        LogLoader("PIPE", "Fatal: ConvertStringSecurityDescriptorToSecurityDescriptor failed.");
        return;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };

    while (!g_exitHost) {
        // Create pipe with Overlapped support
        HANDLE hPipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            LogLoader("PIPE", "CreateNamedPipeW failed. Retrying in 1s.");
            Sleep(1000);
            continue;
        }

        SafeHandle shPipe(hPipe);

        SafeHandle hConnectEvent(CreateEventW(NULL, TRUE, FALSE, NULL));
        OVERLAPPED ovConnect = { 0 };
        ovConnect.hEvent = hConnectEvent.Get();

        BOOL connected = ConnectNamedPipe(shPipe.Get(), &ovConnect);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                while (!g_exitHost) {
                    DWORD waitRes = WaitForSingleObject(hConnectEvent.Get(), 500);
                    if (waitRes == WAIT_OBJECT_0) {
                        connected = TRUE;
                        break;
                    }
                }
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }

        if (!connected || g_exitHost) {
            continue;
        }

        LogLoader("PIPE", "Agent connected. Overlapped read session started.");

        static constexpr DWORD PIPE_BUF_BYTES = 65536;
        std::vector<char> rawBuf(PIPE_BUF_BYTES);
        SafeHandle hReadEvent(CreateEventW(NULL, TRUE, FALSE, NULL));

        while (!g_exitHost) {
            OVERLAPPED ovRead = { 0 };
            ovRead.hEvent = hReadEvent.Get();
            DWORD bytesRead = 0;

            BOOL readOk = ReadFile(
                shPipe.Get(),
                rawBuf.data(),
                static_cast<DWORD>(rawBuf.size() - 1),
                &bytesRead,
                &ovRead
            );

            if (!readOk) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    // Timeout set to 10 seconds for Zombie connection mitigation
                    DWORD waitRes = WaitForSingleObject(hReadEvent.Get(), 10000);
                    if (waitRes == WAIT_OBJECT_0) {
                        if (GetOverlappedResult(shPipe.Get(), &ovRead, &bytesRead, FALSE)) {
                            readOk = TRUE;
                        } else {
                            readOk = FALSE;
                        }
                    } else if (waitRes == WAIT_TIMEOUT) {
                        LogLoader("PIPE", "Read timeout (zombie connection). Force reconnecting.");
                        break;
                    } else {
                        readOk = FALSE;
                    }
                } else {
                    readOk = FALSE;
                }
            }

            if (!readOk || bytesRead == 0) {
                LogLoader("PIPE", "Pipe closed or agent disconnected.");
                break;
            }

            rawBuf[bytesRead] = '\0';
            const DWORD64 recvTick = GetTickCount64();

            // Push raw packet to queue (Zero-latency on IPC thread)
            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                g_packetQueue.push_back({ std::string(rawBuf.data(), bytesRead), recvTick });
                g_queueCv.notify_one();
            }
        }

        DisconnectNamedPipe(shPipe.Get());
        
        // Reset splitter safely on disconnect
        {
            std::lock_guard<std::mutex> lock(g_csMutex);
            g_splitter.Reset();
        }
    }

    if (pSD) {
        LocalFree(pSD);
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

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return 0;

    typedef HANDLE(WINAPI* CreateSnapshot_t)(DWORD, DWORD);
    typedef BOOL(WINAPI* ProcessFirst_t)(HANDLE, LPPROCESSENTRY32W);
    typedef BOOL(WINAPI* ProcessNext_t)(HANDLE, LPPROCESSENTRY32W);

    char p1[] = { 'C','r','e','a','t','e','T','o','o','l','h','e','l','p','3','2','S','n','a','p','s','h','o','t',0 };
    char p2[] = { 'P','r','o','c','e','s','s','3','2','F','i','r','s','t','W',0 };
    char p3[] = { 'P','r','o','c','e','s','s','3','2','N','e','x','t','W',0 };

    auto pCreateSnapshot = (CreateSnapshot_t)GetProcAddress(hKernel32, p1);
    auto pProcessFirst    = (ProcessFirst_t)GetProcAddress(hKernel32, p2);
    auto pProcessNext     = (ProcessNext_t)GetProcAddress(hKernel32, p3);

    if (!pCreateSnapshot || !pProcessFirst || !pProcessNext) return 0;

    HANDLE snapshot = pCreateSnapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = { sizeof(entry) };
        if (pProcessFirst(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, processName) == 0) {
                    pid = entry.th32ProcessID;
                    break;
                }
            } while (pProcessNext(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    return pid;
}

// Check if Agent.dll is already loaded in target process
bool IsDLLAlreadyInjected(DWORD pid, const std::wstring& dllName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    MODULEENTRY32W me = { sizeof(me) };
    bool found = false;
    if (Module32FirstW(hSnapshot, &me)) {
        do {
            if (_wcsicmp(me.szModule, dllName.c_str()) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(hSnapshot, &me));
    }
    CloseHandle(hSnapshot);
    return found;
}

bool InjectDLL(DWORD pid, const std::wstring& dllPath) {
    SetAppContainerPermission(dllPath);

    DWORD dwDesiredAccess = PROCESS_CREATE_THREAD | 
                            PROCESS_QUERY_INFORMATION | 
                            PROCESS_VM_OPERATION | 
                            PROCESS_VM_WRITE | 
                            PROCESS_VM_READ;

    SafeHandle shProcess(OpenProcess(dwDesiredAccess, FALSE, pid));
    if (!shProcess.IsValid()) {
        LogLoader("INJECT", "OpenProcess failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hKernel32) return false;

    typedef LPVOID(WINAPI* VirtualAllocEx_t)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
    typedef BOOL(WINAPI* WriteProcessMemory_t)(HANDLE, LPVOID, LPCVOID, SIZE_T, SIZE_T*);
    typedef HANDLE(WINAPI* CreateRemoteThread_t)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);

    char v_a_e[] = { 'V','i','r','t','u','a','l','A','l','l','o','c','E','x',0 };
    char w_p_m[] = { 'W','r','i','t','e','P','r','o','c','e','s','s','M','e','m','o','r','y',0 };
    char c_r_t[] = { 'C','r','e','a','t','e','R','e','m','o','t','e','T','h','r','e','a','d',0 };
    char l_l_w[] = { 'L','o','a','d','L','i','b','r','a','r','y','W',0 };

    auto pVirtualAllocEx    = (VirtualAllocEx_t)GetProcAddress(hKernel32, v_a_e);
    auto pWriteProcessMemory = (WriteProcessMemory_t)GetProcAddress(hKernel32, w_p_m);
    auto pCreateRemoteThread = (CreateRemoteThread_t)GetProcAddress(hKernel32, c_r_t);
    auto pLoadLibraryW       = GetProcAddress(hKernel32, l_l_w);

    if (!pVirtualAllocEx || !pWriteProcessMemory || !pCreateRemoteThread || !pLoadLibraryW) {
        return false;
    }

    const size_t allocSize = (dllPath.length() + 1) * sizeof(wchar_t);

    // RAII for remote memory management
    struct RemoteAllocDeleter {
        HANDLE hProc;
        void operator()(void* ptr) const { if (ptr) VirtualFreeEx(hProc, ptr, 0, MEM_RELEASE); }
    };

    void* remoteMem = pVirtualAllocEx(shProcess.Get(), nullptr, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        LogLoader("INJECT", "VirtualAllocEx failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }
    std::unique_ptr<void, RemoteAllocDeleter> remoteMemGuard(remoteMem, RemoteAllocDeleter{ shProcess.Get() });

    if (!pWriteProcessMemory(shProcess.Get(), remoteMem, dllPath.c_str(), allocSize, nullptr)) {
        LogLoader("INJECT", "WriteProcessMemory failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    SafeHandle shThread(pCreateRemoteThread(shProcess.Get(), nullptr, 0,
                                           reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
                                           remoteMem, 0, nullptr));
    if (!shThread.IsValid()) {
        LogLoader("INJECT", "CreateRemoteThread failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    WaitForSingleObject(shThread.Get(), INFINITE);
    return true;
}

// =============================================================
// MAIN ENTRY & UI CONSUMER LOOP
// =============================================================
int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    LogLoader("SESSION", "=== Host started ===");

    // Adjust Console Window Size
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SMALL_RECT windowSize = { 0, 0,
                              static_cast<SHORT>(TOTAL_W - 1),
                              static_cast<SHORT>(CONSOLE_H - 1) };
    COORD bufferSize     = { static_cast<SHORT>(TOTAL_W),
                             static_cast<SHORT>(CONSOLE_H) };
    SetConsoleScreenBufferSize(hOut, bufferSize);
    SetConsoleWindowInfo(hOut, TRUE, &windowSize);

    // Hide Cursor
    CONSOLE_CURSOR_INFO ci = { 1, FALSE };
    SetConsoleCursorInfo(hOut, &ci);

    DrawFrame();

    // Start asynchronous Overlapped Named Pipe listener on background thread
    std::thread pipeServerThread(PipeListener);
    pipeServerThread.detach();

    MoveConsoleCursor(0, CONSOLE_H - 1);
    std::wcout << L"[*] Waiting for LiveCaptions.exe...";

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring strPath(exePath);
    std::wstring dllPath = strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\Agent.dll";

    DWORD pid = 0;
    bool settingsOpened = false;

    // Discovery Loop (Idempotent Settings Opening)
    while (true) {
        pid = GetProcessIdByName(TARGET_APP);
        if (pid != 0) {
            MoveConsoleCursor(0, CONSOLE_H - 1);
            ClearPanelLine(0, CONSOLE_H - 1, TOTAL_W);
            std::wcout << L"[+] LiveCaptions detected (PID: " << pid << L"). Injecting...";
            
            // Check if Agent.dll is already injected
            if (IsDLLAlreadyInjected(pid, L"Agent.dll")) {
                MoveConsoleCursor(0, CONSOLE_H - 1);
                ClearPanelLine(0, CONSOLE_H - 1, TOTAL_W);
                std::wcout << L"[+] Agent.dll already injected. Listening for captions...";
                break;
            }

            if (InjectDLL(pid, dllPath)) {
                MoveConsoleCursor(0, CONSOLE_H - 1);
                ClearPanelLine(0, CONSOLE_H - 1, TOTAL_W);
                std::wcout << L"[+] Agent.dll injected successfully. Listening...";
                break;
            } else {
                MoveConsoleCursor(0, CONSOLE_H - 1);
                ClearPanelLine(0, CONSOLE_H - 1, TOTAL_W);
                std::wcout << L"[-] Injection failed. Retrying in 2s...";
                Sleep(2000);
            }
        } else {
            // Idempotent open settings: Only open settings once!
            if (!settingsOpened) {
                // Open Live Captions settings page
                ShellExecuteW(NULL, L"open", L"ms-settings:privacy-livecaptions", NULL, NULL, SW_SHOWNORMAL);
                settingsOpened = true;
                LogLoader("DISCOVERY", "ShellExecute initiated to open Live Captions Settings.");
            }
            Sleep(2000);
        }
    }

    // Consumer Loop: Handles UTF-16 Conversion, JSON Parsing, Logic and rendering
    while (!g_exitHost) {
        RawPacket rawPkt;
        {
            std::unique_lock<std::mutex> lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return !g_packetQueue.empty() || g_exitHost; });

            if (g_exitHost && g_packetQueue.empty()) {
                break;
            }

            rawPkt = g_packetQueue.front();
            g_packetQueue.pop_front();
        }

        // Convert UTF-8 payload to Wide String
        int wideLen = MultiByteToWideChar(CP_UTF8, 0, rawPkt.data.c_str(), -1, nullptr, 0);
        if (wideLen <= 0) continue;

        std::vector<wchar_t> wideBuf(wideLen);
        MultiByteToWideChar(CP_UTF8, 0, rawPkt.data.c_str(), -1, wideBuf.data(), wideLen);

        PipePacket pkt;
        if (!ParsePacket(wideBuf.data(), pkt) || pkt.text.empty()) {
            continue;
        }

        // Latency computation (safe guard against tiny clock skew)
        const DWORD64 delayMs = (pkt.ts_ms > 0 && rawPkt.recvTick >= pkt.ts_ms)
            ? (rawPkt.recvTick - pkt.ts_ms)
            : 0;

        LogLoader("PKT", std::string(pkt.is_final ? "FINAL  " : "PARTIAL") + 
                  " text_len=" + std::to_string(pkt.text.size()) + 
                  " delay=" + std::to_string(delayMs) + "ms");

        {
            std::lock_guard<std::mutex> lock(g_csMutex);

            g_pktCount++;
            g_totalBytes  += pkt.bytes;
            g_lastDelayMs  = delayMs;
            FormatTimestamp(rawPkt.recvTick, g_lastTs, 20);

            // Left panel: LIVE STREAM
            const std::wstring livePrefix = pkt.is_final ? L"[F] " : L"[~] ";
            WritePanelText(static_cast<SHORT>(COL_LIVE_X), g_liveRow, COL_LIVE_W, livePrefix + pkt.text);
            RedrawDividers(g_liveRow);
            g_liveRow++;
            if (g_liveRow >= CONSOLE_H - 1) g_liveRow = CONTENT_TOP;

            // Center panel: CONFIRMED SENTENCES (Sentence Splitter)
            auto sentences = g_splitter.ExtractNewSentences(pkt.text, pkt.is_final);
            for (const std::wstring& s : sentences) {
                ++g_splitter.sentence_idx;
                std::wstring label = std::to_wstring(g_splitter.sentence_idx) + L". ";
                WritePanelText(static_cast<SHORT>(COL_CONFIRM_X), g_confirmRow, COL_CONFIRM_W, label + s);
                RedrawDividers(g_confirmRow);
                g_confirmRow++;
                if (g_confirmRow >= CONSOLE_H - 1) g_confirmRow = CONTENT_TOP;
            }

            // Right panel: STATS
            UpdateStatsPanel();
        }
    }

    g_exitHost = true;
    g_queueCv.notify_all();
    return 0;
}
