#pragma once
#include <Windows.h>
#include <string>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "SafeHandle.h"

struct RawPacket {
    std::string data;
    DWORD64     recvTick;
};

extern std::deque<RawPacket>   g_packetQueue;
extern std::mutex              g_queueMutex;
extern std::condition_variable g_queueCv;
extern std::atomic<bool>       g_exitHost;
extern std::atomic<HANDLE>     g_hServerPipe;

bool IsProcessAlive(DWORD pid);
void PipeListener();
