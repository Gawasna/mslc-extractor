#include <iostream>
#include <Windows.h>
#include <string>
#include <thread>
#include <vector>
#include <TlHelp32.h>
#include <AclAPI.h>
#include <fcntl.h>
#include <io.h>
#include <sddl.h>
#include <chrono>

using namespace std;

// =============================================================
// 1. LOW-LEVEL CONSOLE ENGINE (WIN32 API)
// =============================================================
const int SCR_W = 100;
const int SCR_H = 30;

struct ConsoleEngine {
    HANDLE hOut;
    CHAR_INFO buffer[SCR_W * SCR_H];
    COORD bSize = { SCR_W, SCR_H };
    COORD bCoord = { 0, 0 };
    SMALL_RECT wRect = { 0, 0, SCR_W - 1, SCR_H - 1 };

    void Init() {
        hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleScreenBufferSize(hOut, bSize);
        Clear();
    }

    void Clear() {
        for (int i = 0; i < SCR_W * SCR_H; ++i) {
            buffer[i].Char.UnicodeChar = L' ';
            buffer[i].Attributes = 7;
        }
    }

    void DrawString(int x, int y, const wstring& str, WORD attr = 7) {
        for (int i = 0; i < (int)str.length() && (x + i) < SCR_W; ++i) {
            int idx = y * SCR_W + (x + i);
            if (idx >= 0 && idx < SCR_W * SCR_H) {
                buffer[idx].Char.UnicodeChar = str[i];
                buffer[idx].Attributes = attr;
            }
        }
    }

    void DrawWrap(int x, int y, int h, const wstring& str, WORD attr = 7) {
        int curX = 0, curY = 0;
        for (wchar_t c : str) {
            if (curY >= h) break;
            int idx = (y + curY) * SCR_W + (x + curX);
            if (idx >= 0 && idx < SCR_W * SCR_H) {
                buffer[idx].Char.UnicodeChar = c;
                buffer[idx].Attributes = attr;
            }
            curX++;
            if (curX >= SCR_W - x - 2) { curX = 0; curY++; }
        }
    }

    void Flush() {
        WriteConsoleOutputW(hOut, buffer, bSize, bCoord, &wRect);
    }
} g_gui;

// =============================================================
// 2. STATE MANAGEMENT & DATA PROCESSING
// =============================================================
vector<wstring> g_history;
wstring g_committedPrefix = L"";
wstring g_activePart = L"";
long long g_packetCount = 0;
double g_lastFrameTime = 0;

void CommitToHistory(const wstring& text) {
    if (text.empty() || text.length() < 3) return;
    wstring entry = (text.length() > 88) ? text.substr(0, 85) + L"..." : text;
    if (!g_history.empty() && g_history.back() == entry) return;
    g_history.push_back(entry);
    if (g_history.size() > 8) g_history.erase(g_history.begin());
}

void UpdateFrame(const wstring& engineSnapshot) {
    auto tStart = chrono::steady_clock::now();
    g_packetCount++;

    // --- BƯỚC 1: XỬ LÝ RESET ---
    if (engineSnapshot.length() < g_committedPrefix.length() * 0.7) {
        if (!g_activePart.empty()) CommitToHistory(g_activePart);
        g_committedPrefix = L"";
        g_activePart = L"";
    }

    // --- BƯỚC 2: TÍNH ACTIVE PART ---
    if (engineSnapshot.length() >= g_committedPrefix.length()) {
        g_activePart = engineSnapshot.substr(g_committedPrefix.length());
    }

    // --- BƯỚC 3: COMMIT NHÂN VĂN (Punctuation-based) ---
    // Không xử lý 15 ký tự cuối cùng (Volatile Tail) vì Engine đang sửa lỗi
    if (g_activePart.length() > 30) {
        wstring safeZone = g_activePart.substr(0, g_activePart.length() - 15);
        size_t lastPunct = safeZone.find_last_of(L".?!;");

        bool shouldCommit = false;
        size_t cutPos = 0;

        if (lastPunct != wstring::npos && lastPunct > 20) {
            // Trường hợp A: Tìm thấy dấu ngắt câu
            shouldCommit = true;
            cutPos = lastPunct + 1;
        }
        else if (g_activePart.length() > 450) {
            // Trường hợp B: Nói quá dài mà không ngắt câu -> Ép "trảm" tại dấu cách
            size_t lastSpace = safeZone.find_last_of(L" ");
            shouldCommit = true;
            cutPos = (lastSpace != wstring::npos) ? lastSpace : 450;
        }

        if (shouldCommit) {
            wstring chunk = g_activePart.substr(0, cutPos);
            CommitToHistory(chunk);
            g_committedPrefix += chunk;
            g_activePart = g_activePart.substr(cutPos);
        }
    }

    // --- BƯỚC 4: RENDERING ---
    g_gui.Clear();
    g_gui.DrawString(0, 0, L" [ NATIVE PRO STREAMER ] - HUMANE COMMIT ENGINE", 11);
    g_gui.DrawString(0, 1, L" Pkts: " + to_wstring(g_packetCount) +
        L" | Live: " + to_wstring(g_activePart.length()) +
        L" | Render: " + to_wstring(g_lastFrameTime) + L"ms", 10);
    g_gui.DrawString(0, 2, L"--------------------------------------------------------------------------------", 8);

    g_gui.DrawString(0, 4, L" ACTIVE CAPTION (REAL-TIME):", 14);
    g_gui.DrawWrap(2, 5, 10, g_activePart, 15);

    g_gui.DrawString(0, 16, L" RECENT HISTORY (COMMITTED):", 14);
    for (int i = 0; i < (int)g_history.size(); ++i) {
        g_gui.DrawString(2, 17 + i, L"> " + g_history[i], 8);
    }
    g_gui.Flush();

    auto tEnd = chrono::steady_clock::now();
    g_lastFrameTime = chrono::duration<double, std::milli>(tEnd - tStart).count();
}

// =============================================================
// 3. SYSTEM & INJECTION
// =============================================================
void PipeServerThread() {
    SECURITY_DESCRIPTOR sd; InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), &sd, FALSE };
    while (true) {
        HANDLE hPipe = CreateNamedPipeW(L"\\\\.\\pipe\\LiveCaptionPipe", PIPE_ACCESS_INBOUND,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 65536, 65536, 0, &sa);
        if (hPipe != INVALID_HANDLE_VALUE) {
            if (ConnectNamedPipe(hPipe, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) {
                vector<wchar_t> buffer(32768);
                DWORD read;
                if (ReadFile(hPipe, buffer.data(), (DWORD)buffer.size() * 2, &read, NULL)) {
                    UpdateFrame(wstring(buffer.data(), read / 2));
                }
            }
            DisconnectNamedPipe(hPipe); CloseHandle(hPipe);
        }
    }
}

void SetAppContainerPermission(const wstring& path) {
    PACL pOldDACL = NULL, pNewDACL = NULL; PSECURITY_DESCRIPTOR pSD = NULL; EXPLICIT_ACCESSW ea = { 0 };
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &pOldDACL, NULL, &pSD) == ERROR_SUCCESS) {
        BuildExplicitAccessWithNameW(&ea, (LPWSTR)L"ALL APPLICATION PACKAGES", GENERIC_READ | GENERIC_EXECUTE, SET_ACCESS, SUB_CONTAINERS_AND_OBJECTS_INHERIT);
        if (SetEntriesInAclW(1, &ea, pOldDACL, &pNewDACL) == ERROR_SUCCESS) {
            SetNamedSecurityInfoW((LPWSTR)path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, pNewDACL, NULL);
            if (pNewDACL) LocalFree(pNewDACL);
        }
        if (pSD) LocalFree(pSD);
    }
}

DWORD GetPID(const wchar_t* name) {
    DWORD pid = 0; HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry = { sizeof(entry) };
        if (Process32FirstW(snap, &entry)) {
            do { if (_wcsicmp(entry.szExeFile, name) == 0) { pid = entry.th32ProcessID; break; } } while (Process32NextW(snap, &entry));
        }
        CloseHandle(snap);
    }
    return pid;
}

bool Inject(DWORD pid, const wstring& dllPath) {
    SetAppContainerPermission(dllPath);
    HANDLE hProc = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!hProc) return false;
    void* mem = VirtualAllocEx(hProc, nullptr, (dllPath.length() + 1) * 2, MEM_COMMIT, PAGE_READWRITE);
    if (mem) {
        WriteProcessMemory(hProc, mem, dllPath.c_str(), (dllPath.length() + 1) * 2, nullptr);
        HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0, (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"), mem, 0, nullptr);
        if (hThread) { WaitForSingleObject(hThread, 500); CloseHandle(hThread); CloseHandle(hProc); return true; }
    }
    CloseHandle(hProc); return false;
}

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    g_gui.Init();
    thread(PipeServerThread).detach();
    DWORD pid = GetPID(L"LiveCaptions.exe");
    if (pid == 0) {
        ShellExecuteW(NULL, L"open", L"ms-settings:privacy-livecaptions", NULL, NULL, SW_SHOWNORMAL);
        while (pid == 0) { pid = GetPID(L"LiveCaptions.exe"); Sleep(1000); }
    }
    wchar_t path[MAX_PATH]; GetModuleFileNameW(NULL, path, MAX_PATH);
    wstring dllPath = wstring(path).substr(0, wstring(path).find_last_of(L"\\")) + L"\\HookCore.dll";
    Inject(pid, dllPath);
    while (true) Sleep(1000);
    return 0;
}