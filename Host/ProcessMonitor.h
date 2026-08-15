#pragma once
#include <Windows.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------
// Exit codes returned by main()
// ---------------------------------------------------------------
enum class HostExitCode : int {
    Success           = 0,  // Clean exit (Ctrl+C or g_exitHost)
    InjectionFailed   = 1,  // Max injection retries exceeded
    ProcessNotFound   = 2,  // Target not running, --auto-launch not set
    LaunchFailed      = 3,  // --auto-launch set but CreateProcess failed
    DuplicateDetected = 4,  // Multiple instances found (non-fatal warning, first PID used)
    ProcessExited     = 5,  // Watched process exited, --on-exit=quit
    PipeError         = 6,  // Named pipe server failed
    UserAborted       = 7,  // Ctrl+C / graceful signal
};

// OnExitAction is defined in AppConfig.h to avoid circular include.
// ParseOnExitAction is implemented in ProcessMonitor.cpp.
// Include AppConfig.h before this header to use OnExitAction.
#include "AppConfig.h"

// Parse "--on-exit" string to enum (case-insensitive).
// Returns Quit for unknown values.
OnExitAction ParseOnExitAction(const std::string& s);

// ---------------------------------------------------------------
// Enumerate all PIDs matching exeName (for duplicate detection).
// Uses CreateToolhelp32Snapshot — no elevated rights required.
// ---------------------------------------------------------------
std::vector<DWORD> FindAllProcessInstances(const wchar_t* exeName);

// ---------------------------------------------------------------
// Launch LiveCaptions.exe.
// Strategy (in order):
//   1. CreateProcessW from GetSystemDirectoryW (system32)
//   2. ShellExecuteW fallback on same path
//   3. ShellExecuteW URI: shell:AppsFolder (packaged-app path)
// Writes resulting PID to *outPid on success.
// Returns false and sets *outPid = 0 on failure.
// ---------------------------------------------------------------
bool LaunchLiveCaptions(DWORD* outPid);

// ---------------------------------------------------------------
// Resume all suspended threads of a process and attempt to
// restore its window. Used after spawning AppContainer processes
// (e.g. LiveCaptions) that launch headless from a non-UI context.
//
// Strategy:
//   1. Enumerate threads via CreateToolhelp32Snapshot
//   2. ResumeThread on each — idempotent if already running
//   3. EnumWindows to find + ShowWindow(SW_RESTORE) the app window
// ---------------------------------------------------------------
void WakeupSuspendedProcess(DWORD pid);

// ---------------------------------------------------------------
// Block until pid exits OR g_exitHost is set.
// Uses WaitForSingleObject (500ms poll) — no busy spin.
// Returns true  if process exited naturally.
// Returns false if g_exitHost interrupted the wait.
// ---------------------------------------------------------------
bool WatchProcessUntilExit(DWORD pid);
