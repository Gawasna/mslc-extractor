#pragma once
#include <string>
#include <vector>
#include <Windows.h>
#include <mutex>

// =============================================================
// SENTENCE SPLITTER - Delta Watermark State Machine
// =============================================================
struct SentenceSplitter {
    static constexpr wchar_t BOUNDARIES[] = L".?!";

    std::wstring prev_text;
    size_t       confirmed_len;
    int          sentence_idx;
    uint64_t     last_offset;

    SentenceSplitter();
    void Reset();
    std::vector<std::wstring> ExtractNewSentences(const std::wstring& text, bool is_final, uint64_t offset);
};

// =============================================================
// TRANSLATION SEGMENTER - Real-Time Subtitle Segmenter
// =============================================================
struct WordInfo {
    std::wstring text;
    std::wstring clean_text;
    size_t start_pos;
    size_t end_pos;
    bool ends_with_punctuation;
    bool is_conjunction;
};

struct TranslationSegmenter {
    size_t last_commit_pos;
    uint64_t last_offset;
    DWORD64 last_packet_time;
    std::wstring prev_text;
    size_t segment_id;
    
    std::vector<DWORD64> speech_gaps;
    static constexpr size_t MAX_GAPS_HISTORY = 20;

    TranslationSegmenter();
    void Reset();
    DWORD64 GetAdaptiveThreshold() const;
    void AddGap(DWORD64 gap);
};

// =============================================================
// GLOBAL PROCESSOR INSTANCES & APIS
// =============================================================
extern SentenceSplitter g_splitter;
extern TranslationSegmenter g_transSegmenter;
extern std::mutex g_csMutex;

void ProcessTranslationAndSplitting(const std::wstring& pktText, bool isFinal, uint64_t offset, uint64_t duration, DWORD64 tsMs, DWORD64 recvTick, DWORD64 delayMs, DWORD64 pktBytes, const std::wstring& resultId);
void CheckSilenceTimeout(DWORD64 now);
void CheckSilenceTimeoutLocked(DWORD64 now);
