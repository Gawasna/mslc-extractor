#pragma once
#include <Windows.h>
#include <string>

#include "SafeHandle.h"

// =============================================================
// INJECTION & PERMISSION APIS
// =============================================================

bool SetAppContainerPermission(const std::wstring& filePath);
bool SetAppContainerWritePermission(const std::wstring& filePath);
DWORD GetProcessIdByName(const wchar_t* processName);
bool IsDLLAlreadyInjected(DWORD pid, const std::wstring& dllName);
bool InjectDLL(DWORD pid, const std::wstring& dllPath);
