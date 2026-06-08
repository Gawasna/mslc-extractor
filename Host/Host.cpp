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
// CONSTANTS & CLI GLOBALS
// =============================================================
static constexpr const wchar_t* TARGET_APP  = L"LiveCaptions.exe";

static std::atomic<DWORD> g_targetPid{0};
static std::atomic<bool>  g_needReinjection{false};
static std::wstring g_customPipeName = L"\\\\.\\pipe\\LiveCaptionPipe";
static bool g_debugMode = false;
static std::string g_customLogPath = "";
static bool g_stdoutOnly = false;
static bool g_noSpawn = false;
static bool g_injectOnly = false;
static bool g_mockMode = false;

// Log file path determined dynamically
static std::string g_logPath = "";

// No layout constants required for standard logging B2B mode.

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

// Helper to determine root logs path
std::string GetLogPath() {
    if (!g_customLogPath.empty()) {
        return g_customLogPath;
    }

    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
        std::wstring wPath(path);
        size_t pos = wPath.find_last_of(L"\\");
        if (pos != std::wstring::npos) {
            std::wstring dir = wPath.substr(0, pos); // C:\Users\...\x64\Release
            pos = dir.find_last_of(L"\\");
            if (pos != std::wstring::npos) {
                std::wstring root = dir.substr(0, pos); // C:\Users\...\x64
                pos = root.find_last_of(L"\\");
                if (pos != std::wstring::npos) {
                    std::wstring projectRoot = root.substr(0, pos); // C:\Users\...\mslc-extractor
                    std::wstring logFile = projectRoot + L"\\logs\\mslc_host_debug.txt";
                    return std::string(logFile.begin(), logFile.end());
                }
            }
        }
    }
    return "C:\\Users\\Public\\mslc_host_debug.txt"; // Fallback
}

// =============================================================
// HOST DEBUG LOGGER (Thread-Safe)
// =============================================================
static constexpr size_t        LOG_MAX_LINES = 100;
static std::mutex              g_logMutex;
static std::deque<std::string> g_logRing;

void LogHost(const char* category, const std::string& msg) {
    if (g_debugMode) {
        std::cerr << "[" << category << "] " << msg << std::endl;
    }

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
          << std::setfill(' ') // Reset fill character to space
          << std::left << std::setw(8) << category
          << "] "
          << msg;

    std::lock_guard<std::mutex> lock(g_logMutex);
    
    if (!g_debugMode) {
        g_logRing.push_back(entry.str());
        if (g_logRing.size() > LOG_MAX_LINES) g_logRing.pop_front();

        std::ofstream f(g_logPath, std::ios_base::trunc);
        if (f.is_open()) {
            for (const auto& line : g_logRing) f << line << '\n';
        }
    } else {
        std::ofstream f(g_logPath, std::ios_base::app);
        if (f.is_open()) {
            f << entry.str() << '\n';
        }
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
// LOGGING STATE (guarded by g_csMutex)
// =============================================================
static std::mutex g_csMutex;
static DWORD64    g_pktCount    = 0;
static DWORD64    g_totalBytes  = 0;
static DWORD64    g_lastDelayMs = 0;
static wchar_t    g_lastTs[20]  = L"--:--:--";

static size_t     g_lastLiveWidth = 0;

void PrintLiveText(const std::wstring& text) {
    std::wstring line = L"\r[LIVE] [~] " + text;
    if (line.size() < g_lastLiveWidth) {
        line += std::wstring(g_lastLiveWidth - line.size(), L' ');
    }
    g_lastLiveWidth = line.size();
    std::wcout << line;
    std::wcout.flush();
}

void ClearLiveText() {
    if (g_lastLiveWidth > 0) {
        std::wstring clearLine(g_lastLiveWidth, L' ');
        std::wcout << L"\r" << clearLine << L"\r";
        std::wcout.flush();
        g_lastLiveWidth = 0;
    }
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
    DWORD64      offset   = 0;
    DWORD64      duration = 0;
    std::wstring result_id;
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

    p = data.find(L"\"offset\":");
    if (p != std::wstring::npos) {
        out.offset = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 9));
    }

    p = data.find(L"\"duration\":");
    if (p != std::wstring::npos) {
        out.duration = static_cast<DWORD64>(_wtoi64(data.c_str() + p + 11));
    }

    p = data.find(L"\"result_id\":\"");
    if (p != std::wstring::npos) {
        p += 13;
        size_t r_end = data.find(L'"', p);
        if (r_end != std::wstring::npos) {
            out.result_id = data.substr(p, r_end - p);
        }
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
            LogHost("SPLITTER",
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
                LogHost("SPLITTER",
                    "FINAL tail_len=" + std::to_string(tail.size()) +
                    " tail=\"" + TruncateForLog(tail) + "\" -> COMMIT");
                results.push_back(tail);
            }
            confirmed_len = text.size();
            prev_text = text;
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
                        LogHost("EMIT", "Emitting sentence: \"" + TruncateForLog(sentence) + "\"");
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
    // Allow AppContainers (S-1-15-2-1) & Everyone (WD) generic Read/Write.
    // Local System (SY) & Administrators (BA) generic All.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GRGW;;;S-1-15-2-1)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)", 
            SDDL_REVISION_1, 
            &pSD, 
            NULL)) {
        LogHost("PIPE", "Fatal: ConvertStringSecurityDescriptorToSecurityDescriptor failed.");
        return;
    }

    SECURITY_ATTRIBUTES sa = { sizeof(sa), pSD, FALSE };

    while (!g_exitHost) {
        HANDLE hPipe = CreateNamedPipeW(
            g_customPipeName.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1, 65536, 65536, 0, &sa
        );

        if (hPipe == INVALID_HANDLE_VALUE) {
            LogHost("PIPE", "CreateNamedPipeW failed. Retrying in 1s.");
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

        LogHost("PIPE", "Agent connected. Overlapped read session started.");

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
                    DWORD waitRes = WaitForSingleObject(hReadEvent.Get(), 10000);
                    if (waitRes == WAIT_OBJECT_0) {
                        if (GetOverlappedResult(shPipe.Get(), &ovRead, &bytesRead, FALSE)) {
                            readOk = TRUE;
                        } else {
                            readOk = FALSE;
                        }
                    } else if (waitRes == WAIT_TIMEOUT) {
                        LogHost("PIPE", "Read timeout (zombie connection). Force reconnecting.");
                        break;
                    } else {
                        readOk = FALSE;
                    }
                } else {
                    readOk = FALSE;
                }
            }

            if (!readOk || bytesRead == 0) {
                LogHost("PIPE", "Pipe closed or agent disconnected.");
                break;
            }

            rawBuf[bytesRead] = '\0';
            const DWORD64 recvTick = GetTickCount64();

            {
                std::lock_guard<std::mutex> lock(g_queueMutex);
                g_packetQueue.push_back({ std::string(rawBuf.data(), bytesRead), recvTick });
                g_queueCv.notify_one();
            }
        }

        DisconnectNamedPipe(shPipe.Get());
        
        {
            std::lock_guard<std::mutex> lock(g_csMutex);
            g_splitter.Reset();
        }

        // Check if the target process crashed or terminated
        if (!g_mockMode && g_targetPid != 0) {
            SafeHandle shProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_targetPid));
            bool isProcessDead = true;
            if (shProcess.IsValid()) {
                DWORD exitCode = 0;
                if (GetExitCodeProcess(shProcess.Get(), &exitCode)) {
                    if (exitCode == STILL_ACTIVE) {
                        isProcessDead = false;
                    }
                }
            }

            if (isProcessDead) {
                LogHost("PIPE", "LiveCaptions.exe (PID: " + std::to_string(g_targetPid) + ") has terminated or crashed.");
                if (!g_stdoutOnly) {
                    ClearLiveText();
                    std::wcout << L"[-] LiveCaptions.exe (PID: " << g_targetPid << L") terminated or crashed. Re-discovery initiated..." << std::endl;
                }
                g_needReinjection = true;
                g_queueCv.notify_all();
            }
        }
    }

    if (pSD) {
        LocalFree(pSD);
    }
}

// =============================================================
// MOCK CLIENT THREAD (offline UI & Splitter verification)
// =============================================================
void MockClientThread(std::wstring pipeName) {
    LogHost("MOCK", "Mock client thread started.");
    Sleep(1500); // Wait for Pipe Server to initialize
    
    DWORD lastErr = 0;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 10; ++i) {
        hPipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_WRITE,
            0,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }
        lastErr = GetLastError();
        Sleep(500);
    }
    
    if (hPipe == INVALID_HANDLE_VALUE) {
        LogHost("MOCK", "Failed to connect to mock pipe server. Last error: " + std::to_string(lastErr));
        return;
    }
    
    LogHost("MOCK", "Mock client connected to pipe server.");
    
    std::vector<std::string> mockSentences = {
        "Hello, this is a mock subtitle test.",
        "We are verifying the split-view console user interface.",
        "The delta watermark sentence splitter should process this correctly.",
        "It splits incoming text stream by punctuation marks.",
        "Does the mock verification look good?",
        "Yes, it seems to work beautifully!"
    };
    
    for (const auto& sentence : mockSentences) {
        std::stringstream ss(sentence);
        std::string word;
        std::string currentText = "";
        int wordCount = 0;
        while (ss >> word) {
            if (!currentText.empty()) currentText += " ";
            currentText += word;
            wordCount++;
            
            std::string payload = "{\"text\":\"" + currentText + 
                                  "\",\"is_final\":false,\"bytes\":" + std::to_string(currentText.length()) + 
                                  ",\"ts_ms\":" + std::to_string(GetTickCount64()) + 
                                  ",\"offset\":" + std::to_string(wordCount * 10000) + 
                                  ",\"duration\":" + std::to_string(5000) + 
                                  ",\"result_id\":\"mock_id_" + std::to_string(GetTickCount64() % 1000) + "\"}";
            
            DWORD written = 0;
            WriteFile(hPipe, payload.c_str(), static_cast<DWORD>(payload.length()), &written, NULL);
            Sleep(250); // Simulate typing
        }
        
        std::string payload = "{\"text\":\"" + sentence + 
                              "\",\"is_final\":true,\"bytes\":" + std::to_string(sentence.length()) + 
                              ",\"ts_ms\":" + std::to_string(GetTickCount64()) + 
                              ",\"offset\":" + std::to_string(wordCount * 10000) + 
                              ",\"duration\":" + std::to_string(10000) + 
                              ",\"result_id\":\"mock_id_final_" + std::to_string(GetTickCount64() % 1000) + "\"}";
        DWORD written = 0;
        WriteFile(hPipe, payload.c_str(), static_cast<DWORD>(payload.length()), &written, NULL);
        Sleep(1000); // Interval between sentences
    }
    
    CloseHandle(hPipe);
    LogHost("MOCK", "Mock client finished and disconnected.");
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

void SetAppContainerWritePermission(const std::wstring& filePath) {
    PACL pOldDACL = NULL, pNewDACL = NULL;
    PSECURITY_DESCRIPTOR pSD = NULL;
    EXPLICIT_ACCESSW ea = { 0 };
    if (GetNamedSecurityInfoW(filePath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                               NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, const_cast<LPWSTR>(L"ALL APPLICATION PACKAGES"),
                                     GENERIC_READ | GENERIC_WRITE | GENERIC_ALL,
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
        LogHost("INJECT", "OpenProcess failed (err=" + std::to_string(GetLastError()) + ")");
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

    struct RemoteAllocDeleter {
        HANDLE hProc;
        void operator()(void* ptr) const { if (ptr) VirtualFreeEx(hProc, ptr, 0, MEM_RELEASE); }
    };

    void* remoteMem = pVirtualAllocEx(shProcess.Get(), nullptr, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        LogHost("INJECT", "VirtualAllocEx failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }
    std::unique_ptr<void, RemoteAllocDeleter> remoteMemGuard(remoteMem, RemoteAllocDeleter{ shProcess.Get() });

    if (!pWriteProcessMemory(shProcess.Get(), remoteMem, dllPath.c_str(), allocSize, nullptr)) {
        LogHost("INJECT", "WriteProcessMemory failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    SafeHandle shThread(pCreateRemoteThread(shProcess.Get(), nullptr, 0,
                                           reinterpret_cast<LPTHREAD_START_ROUTINE>(pLoadLibraryW),
                                           remoteMem, 0, nullptr));
    if (!shThread.IsValid()) {
        LogHost("INJECT", "CreateRemoteThread failed (err=" + std::to_string(GetLastError()) + ")");
        return false;
    }

    WaitForSingleObject(shThread.Get(), INFINITE);
    return true;
}

// =============================================================
// MAIN ENTRY & UI CONSUMER LOOP
// =============================================================
int main(int argc, char* argv[]) {
    // 1. Parse CLI Arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" || arg == "--pid") {
            if (i + 1 < argc) {
                g_targetPid = static_cast<DWORD>(std::stoul(argv[++i]));
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
        } else if (arg == "-m" || arg == "--mock") {
            g_mockMode = true;
        }
    }

    // 2. Initialize logs path & folder
    g_logPath = GetLogPath();
    size_t lastSlash = g_logPath.find_last_of("\\");
    if (lastSlash != std::string::npos) {
        std::string logDir = g_logPath.substr(0, lastSlash);
        CreateDirectoryA(logDir.c_str(), NULL);
    }

    // Prepare Agent log file and grant write access to AppContainers
    if (!g_mockMode) {
        std::wstring agentLogPath;
        {
            wchar_t path[MAX_PATH];
            if (GetModuleFileNameW(NULL, path, MAX_PATH)) {
                std::wstring wPath(path);
                size_t pos = wPath.find_last_of(L"\\");
                if (pos != std::wstring::npos) {
                    std::wstring dir = wPath.substr(0, pos);
                    pos = dir.find_last_of(L"\\");
                    if (pos != std::wstring::npos) {
                        std::wstring root = dir.substr(0, pos);
                        pos = root.find_last_of(L"\\");
                        if (pos != std::wstring::npos) {
                            std::wstring projectRoot = root.substr(0, pos);
                            agentLogPath = projectRoot + L"\\logs\\mslc_agent_debug.txt";
                        }
                    }
                }
            }
        }
        if (!agentLogPath.empty()) {
            HANDLE hFile = CreateFileW(agentLogPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                CloseHandle(hFile);
                SetAppContainerWritePermission(agentLogPath);
            }
        }
    }

    LogHost("SESSION", "=== Host started ===");

    // 3. UI setup (skipped in --stdout or --inject-only mode)
    if (!g_stdoutOnly && !g_injectOnly) {
        _setmode(_fileno(stdout), _O_U16TEXT);
    }

    // 4. Start Named Pipe Server
    std::thread pipeServerThread(PipeListener);
    pipeServerThread.detach();

    // 5. Start Mock Client Thread if in Mock Mode
    if (g_mockMode) {
        std::thread mockThread(MockClientThread, g_customPipeName);
        mockThread.detach();
    }

    // 6. Main Connection & Lifecycle Loop
    while (!g_exitHost) {
        // Discovery & Injection (skipped in Mock Mode)
        if (!g_mockMode) {
            if (!g_stdoutOnly && !g_injectOnly) {
                std::wcout << L"[*] Waiting for LiveCaptions.exe..." << std::endl;
            }

            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(NULL, exePath, MAX_PATH);
            std::wstring strPath(exePath);
            std::wstring dllPath = strPath.substr(0, strPath.find_last_of(L"\\")) + L"\\Agent.dll";

            DWORD pid = g_targetPid;
            bool settingsOpened = false;
            int injectRetries = 0;

            while (!g_exitHost) {
                if (pid == 0) {
                    pid = GetProcessIdByName(TARGET_APP);
                }

                if (pid != 0) {
                    if (!g_stdoutOnly && !g_injectOnly) {
                        std::wcout << L"[+] LiveCaptions detected (PID: " << pid << L"). Injecting..." << std::endl;
                    }
                    
                    if (IsDLLAlreadyInjected(pid, L"Agent.dll")) {
                        if (!g_stdoutOnly && !g_injectOnly) {
                            std::wcout << L"[+] Agent.dll already injected. Listening..." << std::endl;
                        }
                        g_targetPid = pid;
                        break;
                    }

                    if (InjectDLL(pid, dllPath)) {
                        if (!g_stdoutOnly && !g_injectOnly) {
                            std::wcout << L"[+] Agent.dll injected successfully. Listening..." << std::endl;
                        }
                        g_targetPid = pid;
                        break;
                    } else {
                        injectRetries++;
                        if (injectRetries >= 3) {
                            if (!g_stdoutOnly && !g_injectOnly) {
                                std::wcout << L"[-] Injection failed 3 times. Return feedback only and aborting." << std::endl;
                            }
                            LogHost("INJECT", "Injection failed 3 times. Aborting.");
                            return 1; // Abort process on failure
                        }
                        if (!g_stdoutOnly && !g_injectOnly) {
                            std::wcout << L"[-] Injection failed. Retrying in 2s..." << std::endl;
                        }
                        Sleep(2000);
                    }
                } else {
                    if (g_injectOnly) {
                        Sleep(2000);
                        continue;
                    }

                    if (!g_noSpawn && !settingsOpened) {
                        ShellExecuteW(NULL, L"open", L"ms-settings:privacy-livecaptions", NULL, NULL, SW_SHOWNORMAL);
                        settingsOpened = true;
                        LogHost("DISCOVERY", "ShellExecute initiated to open Live Captions Settings.");
                    }
                    Sleep(2000);
                }
            }

            if (g_injectOnly) {
                LogHost("SESSION", "Inject-only mode completed. Exiting.");
                return 0;
            }
        } else {
            if (!g_stdoutOnly) {
                std::wcout << L"[Mock Mode] Verification UI running..." << std::endl;
            }
        }

        // Reset re-injection flag before entering consumer loop
        g_needReinjection = false;

        // 7. Consumer Loop: Handles UTF-16 Conversion, JSON Parsing, Logic and rendering
        while (!g_exitHost && !g_needReinjection) {
            RawPacket rawPkt;
            {
                std::unique_lock<std::mutex> lock(g_queueMutex);
                g_queueCv.wait_for(lock, std::chrono::milliseconds(500), [] { 
                    return !g_packetQueue.empty() || g_exitHost || g_needReinjection; 
                });

                if (g_exitHost) {
                    break;
                }
                
                if (g_needReinjection) {
                    break;
                }

                if (g_packetQueue.empty()) {
                    continue;
                }

                rawPkt = g_packetQueue.front();
                g_packetQueue.pop_front();
            }

            if (g_stdoutOnly) {
                // Direct stdout JSON streaming
                std::cout << rawPkt.data << std::endl;
                continue;
            }

            int wideLen = MultiByteToWideChar(CP_UTF8, 0, rawPkt.data.c_str(), -1, nullptr, 0);
            if (wideLen <= 0) continue;

            std::vector<wchar_t> wideBuf(wideLen);
            MultiByteToWideChar(CP_UTF8, 0, rawPkt.data.c_str(), -1, wideBuf.data(), wideLen);

            PipePacket pkt;
            if (!ParsePacket(wideBuf.data(), pkt) || pkt.text.empty()) {
                continue;
            }

            const DWORD64 delayMs = (pkt.ts_ms > 0 && rawPkt.recvTick >= pkt.ts_ms)
                ? (rawPkt.recvTick - pkt.ts_ms)
                : 0;

            // Convert result_id to narrow string for logging
            std::string narrowId(pkt.result_id.begin(), pkt.result_id.end());
            LogHost("PKT", std::string(pkt.is_final ? "FINAL  " : "PARTIAL") + 
                      " text_len=" + std::to_string(pkt.text.size()) + 
                      " delay=" + std::to_string(delayMs) + "ms" +
                      " id=" + narrowId +
                      " offset=" + std::to_string(pkt.offset) +
                      " duration=" + std::to_string(pkt.duration));

            {
                std::lock_guard<std::mutex> lock(g_csMutex);

                g_pktCount++;
                g_totalBytes  += pkt.bytes;
                g_lastDelayMs  = delayMs;
                FormatTimestamp(rawPkt.recvTick, g_lastTs, 20);

                // Extract new sentences via Splitter
                auto sentences = g_splitter.ExtractNewSentences(pkt.text, pkt.is_final);
                if (!sentences.empty()) {
                    // Clear the current live line first to avoid character leftover
                    ClearLiveText();

                    for (const std::wstring& s : sentences) {
                        ++g_splitter.sentence_idx;
                        std::wcout << L"[COMMIT] " << g_splitter.sentence_idx << L". " << s << std::endl;
                    }

                    // Output stats immediately after COMMIT
                    const DWORD64 avg = (g_pktCount > 0) ? (g_totalBytes / g_pktCount) : 0;
                    std::wcout << L"[STATS] Pkts: " << g_pktCount
                               << L" | Bytes: " << g_totalBytes
                               << L" | Avg: " << avg << L" B"
                               << L" | Delay: " << g_lastDelayMs << L" ms"
                               << L" | Last: " << g_lastTs
                               << std::endl;
                }

                // Display remaining live text
                if (!pkt.is_final) {
                    std::wstring liveText = pkt.text;
                    if (g_splitter.confirmed_len < liveText.size()) {
                        liveText = liveText.substr(g_splitter.confirmed_len);
                        size_t start = liveText.find_first_not_of(L' ');
                        if (start != std::wstring::npos) {
                            liveText = liveText.substr(start);
                        }
                    } else {
                        liveText.clear();
                    }
                    PrintLiveText(liveText);
                } else {
                    // If it is final, make sure to clean up the live line
                    ClearLiveText();
                }
            }
        }

        // If we exited the consumer loop due to reinjection, reset target PID to trigger re-discovery
        if (g_needReinjection && !g_mockMode) {
            g_targetPid = 0;
        }
    }

    g_exitHost = true;
    g_queueCv.notify_all();
    return 0;
}
