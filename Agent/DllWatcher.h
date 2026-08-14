#pragma once
#include "NtDecls.h"
#include <Windows.h>
#include <atomic>

extern PVOID g_ldrCookie;
extern std::atomic<bool> g_hookInstalled;

void NTAPI DllNotificationCallback(ULONG NotificationReason, PLDR_DLL_NOTIFICATION_DATA NotificationData, PVOID Context);
void RegisterDllWatcher();
void UnregisterDllWatcher();
void TryHookIfAlreadyLoaded();
