#include "pch.h"
#include <Windows.h>
#include <string>
#include "MinHook.h"

// Offset trỏ tới địa chỉ chuỗi văn bản trong memory của Engine
#define TEXT_OFFSET 0x190

typedef void* (__fastcall* GetDisplayText_t)(void* rcx, void* rdx, void* r8);
GetDisplayText_t fpOriginalGetDisplayText = nullptr;

// Gửi snapshot qua Pipe (Kết nối ngắn hạn để tối ưu độ ổn định)
void SendToPipe(const std::wstring& text) {
    if (text.empty()) return;

    HANDLE hPipe = CreateFileW(L"\\\\.\\pipe\\LiveCaptionPipe", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hPipe != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(hPipe, text.c_str(), (DWORD)(text.length() + 1) * sizeof(wchar_t), &written, NULL);
        CloseHandle(hPipe);
    }
}

void* __fastcall Detour_GetDisplayText(void* rcx, void* rdx, void* r8) {
    void* result = fpOriginalGetDisplayText(rcx, rdx, r8);

    if (rcx != nullptr) {
        wchar_t** ppText = (wchar_t**)((char*)rcx + TEXT_OFFSET);
        if (ppText && *ppText && !IsBadReadPtr(*ppText, 2)) {
            std::wstring currentText(*ppText);
            static std::wstring lastSentText = L"";

            if (currentText != lastSentText) {
                SendToPipe(currentText);
                lastSentText = currentText;
            }
        }
    }
    return result;
}

DWORD WINAPI HookThread(LPVOID lpParam) {
    const wchar_t* targetDll = L"microsoft.cognitiveservices.speech.extension.embedded.sr.runtime.dll";
    HMODULE hModule = nullptr;
    while ((hModule = GetModuleHandleW(targetDll)) == nullptr) Sleep(500);

    // Function đích chịu trách nhiệm render text ra UI của Live Captions
    FARPROC pFunc = GetProcAddress(hModule, "GetUnimicDecoderNBestDisplayText");
    if (pFunc && MH_Initialize() == MH_OK) {
        MH_CreateHook((LPVOID)pFunc, &Detour_GetDisplayText, (LPVOID*)&fpOriginalGetDisplayText);
        MH_EnableHook(MH_ALL_HOOKS);
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD r, LPVOID v) {
    if (r == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)HookThread, NULL, 0, NULL);
    }
    return TRUE;
}