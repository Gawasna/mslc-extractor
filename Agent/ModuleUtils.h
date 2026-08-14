#pragma once
#include <Windows.h>
#include <string>

std::string GetObfuscatedTargetDllName();
HMODULE FindModuleByPartialName(const std::string& partialName);
uint64_t GetPreciseTimeTicks();
