#pragma once
#include <Windows.h>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>

// SDK handle types
typedef void* SPXRESULTHANDLE;
typedef void* SPXRECOHANDLE;
typedef void* SPXEVENTHANDLE;

enum Result_Reason : int {
    ResultReason_NoMatch           = 0,
    ResultReason_Canceled          = 1,
    ResultReason_RecognizingSpeech = 2,
    ResultReason_RecognizedSpeech  = 3,
    ResultReason_TranslatingSpeech = 6,
    ResultReason_TranslatedSpeech  = 7,
};

typedef int(__stdcall* result_get_text_t)     (SPXRESULTHANDLE, char*, uint32_t);
typedef int(__stdcall* result_get_reason_t)   (SPXRESULTHANDLE, int*);
typedef int(__stdcall* result_get_offset_t)   (SPXRESULTHANDLE, uint64_t*);
typedef int(__stdcall* result_get_duration_t) (SPXRESULTHANDLE, uint64_t*);
typedef int(__stdcall* result_get_result_id_t)(SPXRESULTHANDLE, char*, uint32_t);
typedef void(__stdcall* PSESSION_CALLBACK_FUNC)(SPXRECOHANDLE, SPXEVENTHANDLE, void*);
typedef int(__stdcall* recognizer_session_started_set_callback_t)(SPXRECOHANDLE, PSESSION_CALLBACK_FUNC, void*);

extern result_get_text_t      fpOriginalResultGetText;
extern result_get_reason_t    fpOriginalResultGetReason;
extern result_get_offset_t    fpOriginalResultGetOffset;
extern result_get_duration_t  fpOriginalResultGetDuration;
extern result_get_result_id_t fpOriginalResultGetResultId;
extern recognizer_session_started_set_callback_t fpOriginalRecognizerSessionStartedSetCallback;
extern std::mutex g_sessionStartedMutex;
extern std::unordered_map<SPXRECOHANDLE, PSESSION_CALLBACK_FUNC> g_sessionStartedCallbacks;
extern std::atomic<bool> g_sessionStartedEmitted;

void EnsureSessionStartedEmitted(SPXRECOHANDLE hreco, SPXEVENTHANDLE hevent);
void __stdcall DetourSessionStartedCallback(SPXRECOHANDLE hreco, SPXEVENTHANDLE hevent, void* pvContext);
int  __stdcall Detour_recognizer_session_started_set_callback(SPXRECOHANDLE hreco, PSESSION_CALLBACK_FUNC callback, void* pvContext);
int  __stdcall Detour_result_get_text(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen);
DWORD WINAPI HookThread(LPVOID lpParam);
