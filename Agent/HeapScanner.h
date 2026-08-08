#pragma once
#include "pch.h"
#include <Windows.h>
#include <vector>
#include <string>
#include <cstdint>

struct HeapScanResult {
    void* handleAddress;        // SPXRECOHANDLE candidate address
    uintptr_t vtableAddress;     // VTable address inside core DLL
    bool isValid;                // Tier-3 validation result
};

// Main Heap Scanning Interface
__declspec(dllexport) std::vector<HeapScanResult> ScanHeapForRecognizerHandle();

// Helper to format scan results into JSON event payload
std::string BuildHeapScanJsonPayload(const HeapScanResult& result);
