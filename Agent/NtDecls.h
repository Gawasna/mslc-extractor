#pragma once
#include <Windows.h>

typedef LONG NTSTATUS;

#ifndef NTAPI
#define NTAPI __stdcall
#endif

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef const UNICODE_STRING* PCUNICODE_STRING;

typedef struct _LDR_DLL_NOTIFICATION_DATA {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

typedef VOID (NTAPI *PLDR_DLL_NOTIFICATION_FUNCTION)(
    ULONG NotificationReason,
    PLDR_DLL_NOTIFICATION_DATA NotificationData,
    PVOID Context
);

typedef NTSTATUS (NTAPI *PLDR_REGISTER_DLL_NOTIFICATION)(
    ULONG Flags,
    PLDR_DLL_NOTIFICATION_FUNCTION NotificationFunction,
    PVOID Context,
    PVOID *Cookie
);

typedef NTSTATUS (NTAPI *PLDR_UNREGISTER_DLL_NOTIFICATION)(
    PVOID Cookie
);

#define LDR_DLL_NOTIFICATION_REASON_LOADED 1
