#include "pch.h"
#include "DllWatcher.h"
#include "AgentLogger.h"
#include "SdkHooks.h"
#include "ModuleUtils.h"

PVOID g_ldrCookie = nullptr;
std::atomic<bool> g_hookInstalled{false};

VOID NTAPI DllNotificationCallback(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID /*Context*/) {
    if (NotificationReason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
        if (NotificationData && NotificationData->BaseDllName && NotificationData->BaseDllName->Buffer) {
            std::wstring dllName(NotificationData->BaseDllName->Buffer, NotificationData->BaseDllName->Length / sizeof(wchar_t));
            std::transform(dllName.begin(), dllName.end(), dllName.begin(), ::tolower);
            
            if (dllName.find(L"microsoft.cognitiveservices.speech.core.dll") != std::wstring::npos) {
                if (!g_hookInstalled.exchange(true)) {
                    LogInfo("DllNotification: Target DLL loaded. Launching HookThread.");
                    // Start hook thread outside of Windows Loader Lock
                    CreateThread(NULL, 0, HookThread, NotificationData->DllBase, 0, NULL);
                }
            }
        }
    }
}

void RegisterDllWatcher() {
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll) {
        auto pLdrRegisterDllNotification = reinterpret_cast<PLDR_REGISTER_DLL_NOTIFICATION>(
            GetProcAddress(hNtDll, "LdrRegisterDllNotification")
        );
        if (pLdrRegisterDllNotification) {
            pLdrRegisterDllNotification(0, DllNotificationCallback, NULL, &g_ldrCookie);
            LogInfo("DllMain: Registered DLL notification callback.");
        }
    }
}

void UnregisterDllWatcher() {
    HMODULE hNtDll = GetModuleHandleW(L"ntdll.dll");
    if (hNtDll && g_ldrCookie) {
        auto pLdrUnregisterDllNotification = reinterpret_cast<PLDR_UNREGISTER_DLL_NOTIFICATION>(
            GetProcAddress(hNtDll, "LdrUnregisterDllNotification")
        );
        if (pLdrUnregisterDllNotification) {
            pLdrUnregisterDllNotification(g_ldrCookie);
        }
    }
}

void TryHookIfAlreadyLoaded() {
    HMODULE hCoreDLL = FindModuleByPartialName(GetObfuscatedTargetDllName());
    if (hCoreDLL != nullptr) {
        if (!g_hookInstalled.exchange(true)) {
            LogInfo("DllMain: Core DLL already loaded. Launching HookThread.");
            CreateThread(NULL, 0, HookThread, hCoreDLL, 0, NULL);
        }
    }
}
