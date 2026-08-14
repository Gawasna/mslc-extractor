#include "pch.h"
#include "HeapScanner.h"
#include <Psapi.h>
#include <sstream>
#include <iomanip>
#include <algorithm>

#include "AgentLogger.h"
#include "PipeSender.h"
#include "ModuleUtils.h"

static std::string AddressToHexString(uintptr_t addr) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(16) << addr;
    return ss.str();
}

extern std::atomic<bool> g_sessionStartedEmitted;

std::string BuildHeapScanJsonPayload(const HeapScanResult& result) {
    std::ostringstream json;
    json << "{\"event\":\"session_started\""
         << ",\"precise_ticks\":" << GetPreciseTimeTicks()
         << ",\"ts_ms\":" << GetTickCount64()
         << ",\"hreco\":\"" << AddressToHexString(reinterpret_cast<uintptr_t>(result.handleAddress)) << "\""
         << ",\"hevent\":\"" << AddressToHexString(result.vtableAddress) << "\""
         << ",\"is_valid\":" << (result.isValid ? "true" : "false")
         << "}\n";
    return json.str();
}

// Pure SEH helper with no C++ object unwinding (POD parameters only)
static bool SafeProbeCandidate(uintptr_t ptr, uintptr_t dllBase, uintptr_t dllEnd, uintptr_t* outVtable) {
    __try {
        const uintptr_t candidateVtable = *reinterpret_cast<const uintptr_t*>(ptr);

        // Tier-1: Check if candidateVtable falls within core DLL bounds
        if (candidateVtable >= dllBase && candidateVtable < dllEnd) {
            // Tier-2: Dereference VTable entries to check virtual function pointers
            const uintptr_t fn0 = *reinterpret_cast<const uintptr_t*>(candidateVtable);
            const uintptr_t fn1 = *reinterpret_cast<const uintptr_t*>(candidateVtable + sizeof(uintptr_t));

            if (fn0 >= dllBase && fn0 < dllEnd && fn1 >= dllBase && fn1 < dllEnd) {
                *outVtable = candidateVtable;
                return true;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Safe guard against dynamic heap memory reallocations during scan
    }
    return false;
}

__declspec(dllexport) std::vector<HeapScanResult> ScanHeapForRecognizerHandle() {
    std::vector<HeapScanResult> results;

    // 1. Resolve Target Core DLL (microsoft.cognitiveservices.speech.core.dll)
    char p1[] = { 'm','i','c','r','o','s','o','f','t','.',0 };
    char p2[] = { 'c','o','g','n','i','t','i','v','e','s','e','r','v','i','c','e','s','.',0 };
    char p3[] = { 's','p','e','e','c','h','.',0 };
    char p4[] = { 'c','o','r','e','.',0 };
    char p5[] = { 'd','l','l',0 };
    const std::string targetDll = std::string(p1) + p2 + p3 + p4 + p5;

    HMODULE hCoreDLL = FindModuleByPartialName(targetDll);
    if (!hCoreDLL) {
        LogError("HeapScanner: Target DLL 'microsoft.cognitiveservices.speech.core.dll' not loaded in process.");
        return results;
    }

    MODULEINFO modInfo = { 0 };
    if (!GetModuleInformation(GetCurrentProcess(), hCoreDLL, &modInfo, sizeof(modInfo))) {
        LogError("HeapScanner: GetModuleInformation failed.");
        return results;
    }

    const uintptr_t dllBase = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
    const uintptr_t dllEnd  = dllBase + modInfo.SizeOfImage;

    LogInfo("HeapScanner: Initializing heap memory scan. Target DLL base: " + 
            AddressToHexString(dllBase) + ", end: " + AddressToHexString(dllEnd));

    // 2. Query System Address Range
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uintptr_t currentAddr = reinterpret_cast<uintptr_t>(sysInfo.lpMinimumApplicationAddress);
    const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(sysInfo.lpMaximumApplicationAddress);

    MEMORY_BASIC_INFORMATION mbi;
    size_t regionsScanned = 0;
    size_t candidateCount = 0;

    // 3. Main VirtualQuery Loop
    while (currentAddr < maxAddr) {
        if (VirtualQuery(reinterpret_cast<LPCVOID>(currentAddr), &mbi, sizeof(mbi)) == 0) {
            break;
        }

        const bool isCommitted = (mbi.State == MEM_COMMIT);
        const bool isReadWrite = (mbi.Protect & PAGE_READWRITE) || (mbi.Protect & PAGE_EXECUTE_READWRITE);
        const bool isGuard     = (mbi.Protect & PAGE_GUARD) != 0;
        const bool isNoAccess  = (mbi.Protect & PAGE_NOACCESS) != 0;

        if (isCommitted && isReadWrite && !isGuard && !isNoAccess) {
            regionsScanned++;
            const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const size_t regionSize     = mbi.RegionSize;
            const uintptr_t regionEnd   = regionStart + regionSize;

            // 4. Scan 8-byte aligned memory pointers
            for (uintptr_t ptr = regionStart; ptr <= regionEnd - sizeof(uintptr_t); ptr += sizeof(uintptr_t)) {
                uintptr_t candidateVtable = 0;
                if (SafeProbeCandidate(ptr, dllBase, dllEnd, &candidateVtable)) {
                    candidateCount++;
                    HeapScanResult res;
                    res.handleAddress = reinterpret_cast<void*>(ptr);
                    res.vtableAddress = candidateVtable;
                    res.isValid       = true;

                    results.push_back(res);

                    LogInfo("HeapScanner: Discovered valid SPXRECOHANDLE candidate #" + 
                            std::to_string(candidateCount) + " at " + AddressToHexString(ptr) + 
                            " (VTable: " + AddressToHexString(candidateVtable) + ")");

                    // 5. Emit IPC Payload immediately over Pipe
                    const std::string jsonPayload = BuildHeapScanJsonPayload(res);
                    PushToQueue(jsonPayload);
                    g_sessionStartedEmitted.store(true);
                    // Only need one handle, we can stop the scan here to save CPU
                    break;
                }
            }
        }
        
        if (g_sessionStartedEmitted.load()) break;

        // Advance to next memory region
        currentAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }

    LogInfo("HeapScanner: Scan completed. Scanned " + std::to_string(regionsScanned) + 
            " committed regions. Total candidates found: " + std::to_string(results.size()));

    return results;
}
