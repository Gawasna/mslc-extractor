#include "pch.h"
#include "ModuleUtils.h"
#include <Psapi.h>
#include <vector>
#include <algorithm>

std::string GetObfuscatedTargetDllName() {
    char p1[] = { 'm','i','c','r','o','s','o','f','t','.',0 };
    char p2[] = { 'c','o','g','n','i','t','i','v','e','s','e','r','v','i','c','e','s','.',0 };
    char p3[] = { 's','p','e','e','c','h','.',0 };
    char p4[] = { 'c','o','r','e','.',0 };
    char p5[] = { 'd','l','l',0 };
    return std::string(p1) + p2 + p3 + p4 + p5;
}

HMODULE FindModuleByPartialName(const std::string& partialName) {
    HANDLE hProcess = GetCurrentProcess();
    DWORD cbNeeded  = 0;

    EnumProcessModules(hProcess, nullptr, 0, &cbNeeded);
    if (cbNeeded == 0) return nullptr;

    std::vector<HMODULE> hMods(cbNeeded / sizeof(HMODULE));
    if (!EnumProcessModules(hProcess, hMods.data(), cbNeeded, &cbNeeded)) {
        return nullptr;
    }

    std::string partialLower = partialName;
    std::transform(partialLower.begin(), partialLower.end(), partialLower.begin(), ::tolower);

    for (HMODULE hMod : hMods) {
        TCHAR szModName[MAX_PATH] = {};
        if (!GetModuleFileNameEx(hProcess, hMod, szModName, MAX_PATH)) continue;

        std::wstring wName(szModName);
        std::string  sName;
        sName.reserve(wName.size());
        for (wchar_t wc : wName) {
            sName.push_back(static_cast<char>(wc));
        }
        std::transform(sName.begin(), sName.end(), sName.begin(), ::tolower);

        if (sName.find(partialLower) != std::string::npos) {
            return hMod;
        }
    }
    return nullptr;
}

uint64_t GetPreciseTimeTicks() {
    uint64_t ticks = 0;
    GetSystemTimePreciseAsFileTime(reinterpret_cast<FILETIME*>(&ticks));
    return ticks;
}
