#pragma once
#include "SafeHandle.h"
#include <Windows.h>
#include <deque>
#include <string>
#include <mutex>
#include <condition_variable>
#include <atomic>

extern HANDLE g_hPipe;
extern std::deque<std::string> g_sendQueue;
extern std::mutex g_queueMutex;
extern std::condition_variable g_queueCv;
extern std::atomic<bool> g_exitSender;
extern SafeHandle g_hSenderThread;

void PushToQueue(const std::string& payload);
DWORD WINAPI SenderThread(LPVOID lpParam);
