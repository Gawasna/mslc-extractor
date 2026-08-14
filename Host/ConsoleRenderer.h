#pragma once
#include <string>
#include <Windows.h>

std::string WideToUTF8(const std::wstring& wstr);
void PrintLiveText(const std::wstring& text);
void ClearLiveText();
void FormatTimestamp(DWORD64 ts_ms, wchar_t* buf, size_t bufLen);
