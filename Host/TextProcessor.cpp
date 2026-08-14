#include "TextProcessor.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <cwctype>

#include "HostLogger.h"
#include "ConsoleRenderer.h"
#include "AppConfig.h"

DWORD64 g_pktCount = 0;
DWORD64 g_totalBytes = 0;
DWORD64 g_lastDelayMs = 0;
wchar_t g_lastTs[20] = L"--:--:--";

// Global instances
SentenceSplitter g_splitter;
TranslationSegmenter g_transSegmenter;
std::mutex g_csMutex;

// =============================================================
// SENTENCE SPLITTER IMPLEMENTATION
// =============================================================
SentenceSplitter::SentenceSplitter() : confirmed_len(0), sentence_idx(0), last_offset(0) {}

void SentenceSplitter::Reset() {
    prev_text.clear();
    confirmed_len = 0;
    last_offset = 0;
}

std::vector<std::wstring> SentenceSplitter::ExtractNewSentences(const std::wstring& text, bool is_final, uint64_t offset) {
    std::vector<std::wstring> results;

    // Detect segment change via offset timeline
    if (offset != last_offset && last_offset != 0) {
        LogHost("SPLITTER", "New segment detected via offset change: " + 
            std::to_string(last_offset) + " -> " + std::to_string(offset) + " -> RESET watermark");
        Reset();
    }
    last_offset = offset;

    // Calculate LCP to handle retroactive mutation
    if (!prev_text.empty()) {
        size_t min_len = (std::min)(prev_text.size(), text.size());
        size_t lcp = 0;
        while (lcp < min_len && prev_text[lcp] == text[lcp]) {
            lcp++;
        }
        
        if (lcp < prev_text.size()) {
            std::wstring old_tail = prev_text.substr(lcp);
            std::wstring new_tail = text.substr(lcp);
            LogHost("ATOM2", "Splitter Mutation: '" + WideToUTF8(old_tail) + "' -> '" + WideToUTF8(new_tail) + "' (LCP: " + std::to_string(lcp) + ")");
        }

        if (lcp < confirmed_len) {
            LogHost("SPLITTER", "[ATOM2] Danger! Rolling back confirmed_len from " + 
                                std::to_string(confirmed_len) + " to " + std::to_string(lcp));
            confirmed_len = lcp;
        }
    } else if (text.size() < confirmed_len) {
        confirmed_len = text.size();
    }
    prev_text = text;

    if (is_final) {
        std::wstring tail = text.substr(confirmed_len);
        size_t start = tail.find_first_not_of(L' ');
        if (start != std::wstring::npos) tail = tail.substr(start);

        if (!tail.empty()) {
            // Filter punctuation-only tail
            bool has_alnum = false;
            for (wchar_t wc : tail) {
                if (wc > 0x7F || iswalnum(wc)) {
                    has_alnum = true;
                    break;
                }
            }
            if (has_alnum) {
                LogHost("SPLITTER",
                    "FINAL tail_len=" + std::to_string(tail.size()) +
                    " tail=\"" + TruncateForLog(tail) + "\" -> COMMIT");
                results.push_back(tail);
            } else {
                LogHost("SPLITTER", "Filtered empty/punctuation-only final tail: \"" + TruncateForLog(tail) + "\"");
            }
        }
        confirmed_len = text.size();
        prev_text = text;
        return results;
    }

    size_t scan_pos   = confirmed_len;
    size_t commit_pos = confirmed_len;

    while (scan_pos < text.size()) {
        const wchar_t ch = text[scan_pos];
        const bool is_boundary = (ch == L'.' || ch == L'?' || ch == L'!');

        if (is_boundary) {
            const bool at_end            = (scan_pos + 1 >= text.size());
            const bool followed_by_space = !at_end && (text[scan_pos + 1] == L' ');

            if (at_end || followed_by_space) {
                std::wstring sentence = text.substr(commit_pos, scan_pos - commit_pos + 1);
                size_t trim = sentence.find_first_not_of(L' ');
                if (trim != std::wstring::npos && trim > 0) sentence = sentence.substr(trim);

                if (!sentence.empty()) {
                    // Filter out sentences containing only punctuation/spaces
                    bool has_alnum = false;
                    for (wchar_t wc : sentence) {
                        if (wc > 0x7F || iswalnum(wc)) {
                            has_alnum = true;
                            break;
                        }
                    }
                    if (has_alnum) {
                        LogHost("EMIT", "Emitting sentence: \"" + TruncateForLog(sentence) + "\"");
                        results.push_back(sentence);
                    } else {
                        LogHost("SPLITTER", "Filtered empty/punctuation-only sentence: \"" + TruncateForLog(sentence) + "\"");
                    }
                }
                commit_pos = scan_pos + 1;
            }
        }
        ++scan_pos;
    }

    confirmed_len = commit_pos;
    return results;
}

// =============================================================
// TRANSLATION SEGMENTER IMPLEMENTATION
// =============================================================
TranslationSegmenter::TranslationSegmenter() : last_commit_pos(0), last_offset(0), last_packet_time(0), segment_id(0) {}

void TranslationSegmenter::Reset() {
    last_commit_pos = 0;
    prev_text.clear();
}

DWORD64 TranslationSegmenter::GetAdaptiveThreshold() const {
    if (avg_word_history.empty()) {
        return 800; // Default 800ms
    }
    double sum = 0;
    for (double v : avg_word_history) {
        sum += v;
    }
    double mean_ms_per_word = sum / avg_word_history.size();
    
    // A pause is typically 2.5x to 3x the average word duration
    double thresh = mean_ms_per_word * 2.8;
    return static_cast<DWORD64>((std::max)(600.0, (std::min)(1500.0, thresh)));
}

void TranslationSegmenter::UpdatePacing(const std::wstring& text, uint64_t duration) {
    if (duration == 0 || text.empty()) return;
    
    // Tokenize takes time, but we can just do a fast space count for pacing
    size_t word_count = 1;
    for (wchar_t wc : text) {
        if (wc == L' ') word_count++;
    }
    
    if (word_count >= 3) {
        double ms = static_cast<double>(duration) / 10000.0;
        double ms_per_word = ms / word_count;
        
        if (ms_per_word > 100.0 && ms_per_word < 1500.0) {
            avg_word_history.push_back(ms_per_word);
            if (avg_word_history.size() > MAX_WORD_HISTORY) {
                avg_word_history.erase(avg_word_history.begin());
            }
            
            // Log for debugging
            double sum = 0;
            for (double v : avg_word_history) sum += v;
            double mean = sum / avg_word_history.size();
            LogHost("PACING", "Current packet ms/word: " + std::to_string(ms_per_word) + 
                              " | Moving Avg: " + std::to_string(mean) + "ms");
        }
    }
}

// =============================================================
// TEXT HELPERS
// =============================================================
static size_t GetCommonPrefixLength(const std::wstring& s1, const std::wstring& s2) {
    size_t min_len = (std::min)(s1.size(), s2.size());
    size_t i = 0;
    while (i < min_len && s1[i] == s2[i]) {
        i++;
    }
    return i;
}

static bool IsConjunction(const std::wstring& clean_word) {
    static const std::vector<std::wstring> conj = {
        L"and", L"but", L"or", L"so", L"because", L"that", L"which",
        L"và", L"nhưng", L"thì", L"mà", L"nên", L"bởi", L"vì", L"tuy", L"hoặc", L"nếu", L"cho", L"để"
    };
    return std::find(conj.begin(), conj.end(), clean_word) != conj.end();
}

static std::wstring CleanAndLower(const std::wstring& ws, bool& ends_with_punc) {
    std::wstring clean;
    ends_with_punc = false;
    if (ws.empty()) return clean;
    
    wchar_t last_ch = ws.back();
    if (last_ch == L'.' || last_ch == L'?' || last_ch == L'!' || 
        last_ch == L',' || last_ch == L';' || last_ch == L':' || last_ch == L'-') {
        ends_with_punc = true;
    }
    
    for (wchar_t wc : ws) {
        if (wc > 0x7F || iswalnum(wc)) {
            clean.push_back(towlower(wc));
        }
    }
    return clean;
}

static std::vector<WordInfo> Tokenize(const std::wstring& text) {
    std::vector<WordInfo> words;
    size_t i = 0;
    size_t len = text.size();
    
    while (i < len) {
        while (i < len && iswspace(text[i])) {
            i++;
        }
        if (i >= len) break;
        
        size_t start = i;
        while (i < len && !iswspace(text[i])) {
            i++;
        }
        size_t end = i;
        
        std::wstring word_str = text.substr(start, end - start);
        bool ends_punc = false;
        std::wstring clean = CleanAndLower(word_str, ends_punc);
        
        WordInfo wi;
        wi.text = word_str;
        wi.clean_text = clean;
        wi.start_pos = start;
        wi.end_pos = end;
        wi.ends_with_punctuation = ends_punc;
        wi.is_conjunction = IsConjunction(clean);
        
        words.push_back(wi);
    }
    return words;
}

static bool HasAlnum(const std::wstring& s) {
    for (wchar_t wc : s) {
        if (wc > 0x7F || iswalnum(wc)) return true;
    }
    return false;
}

// =============================================================
// TRANSLATION EMISSION
// =============================================================
void EmitTranslateCommit(const std::wstring& type, const std::wstring& text, uint64_t offset, uint64_t duration, DWORD64 ts_ms) {
    g_transSegmenter.segment_id++;
    double offset_sec = static_cast<double>(offset) / 10000000.0;
    double duration_sec = static_cast<double>(duration) / 10000000.0;
    
    ClearLiveText();
    
    std::cout << "[TRANSLATE_COMMIT] [" << WideToUTF8(type) << "] " << g_transSegmenter.segment_id << ". " << WideToUTF8(text)
              << " (offset: " << std::fixed << std::setprecision(2) << offset_sec << "s"
              << ", duration: " << duration_sec << "s"
              << ", ts: " << ts_ms << ")"
              << std::endl;
              
    std::string narrowText(text.begin(), text.end());
    std::string narrowType(type.begin(), type.end());
    LogHost("TRANSLATE", "[" + narrowType + "] Segment " + std::to_string(g_transSegmenter.segment_id) + ": " + narrowText);
}

// =============================================================
// TEXT PROCESSOR CORE APIS
// =============================================================

void ProcessTranslationAndSplitting(const std::wstring& pktText, bool isFinal, uint64_t offset, uint64_t duration, DWORD64 tsMs, DWORD64 recvTick, DWORD64 delayMs, DWORD64 pktBytes, const std::wstring& resultId) {
    std::lock_guard<std::mutex> lock(g_csMutex);

    // --- TRANSLATION SEGMENTATION LOGIC ---
    if (offset != g_transSegmenter.last_offset && g_transSegmenter.last_offset != 0) {
        if (g_transSegmenter.last_commit_pos < g_transSegmenter.prev_text.size()) {
            std::wstring remaining = g_transSegmenter.prev_text.substr(g_transSegmenter.last_commit_pos);
            size_t trim = remaining.find_first_not_of(L' ');
            if (trim != std::wstring::npos) remaining = remaining.substr(trim);
            if (!remaining.empty() && HasAlnum(remaining)) {
                EmitTranslateCommit(L"hard", remaining, g_transSegmenter.last_offset, duration, recvTick);
            }
        }
        g_transSegmenter.Reset();
    } else if (offset == g_transSegmenter.last_offset && !g_transSegmenter.prev_text.empty()) {
        size_t lcp = GetCommonPrefixLength(g_transSegmenter.prev_text, pktText);
        if (lcp < g_transSegmenter.prev_text.size()) {
            std::wstring old_tail = g_transSegmenter.prev_text.substr(lcp);
            std::wstring new_tail = pktText.substr(lcp);
            LogHost("ATOM2", "Segmenter Mutation: '" + WideToUTF8(old_tail) + "' -> '" + WideToUTF8(new_tail) + "' (LCP: " + std::to_string(lcp) + ")");
        }
        if (lcp < g_transSegmenter.last_commit_pos) {
            LogHost("TRANSLATE", "[ATOM2] Danger! Rolling back last_commit_pos from " + 
                                  std::to_string(g_transSegmenter.last_commit_pos) + " to " + std::to_string(lcp));
            g_transSegmenter.last_commit_pos = lcp;
        }
    }
    g_transSegmenter.last_offset = offset;

    DWORD64 now = GetTickCount64();
    if (!isFinal) {
        if (pktText.size() > g_transSegmenter.prev_text.size()) {
            g_transSegmenter.last_packet_time = now;
        } else {
            CheckSilenceTimeoutLocked(now);
        }
    } else {
        g_transSegmenter.last_packet_time = now;
    }
    g_transSegmenter.prev_text = pktText;
    g_transSegmenter.UpdatePacing(pktText, duration);

    if (isFinal) {
        std::wstring remaining = pktText.substr(g_transSegmenter.last_commit_pos);
        size_t trim = remaining.find_first_not_of(L' ');
        if (trim != std::wstring::npos) remaining = remaining.substr(trim);
        if (!remaining.empty() && HasAlnum(remaining)) {
            EmitTranslateCommit(L"hard", remaining, offset, duration, tsMs);
        }
        g_transSegmenter.Reset();
    } else {
        std::wstring remaining = pktText.substr(g_transSegmenter.last_commit_pos);
        auto words = Tokenize(remaining);
        
        size_t cut_pos = std::wstring::npos;
        std::wstring commit_type = L"";
        
        if (words.size() > 20) {
            cut_pos = words[19].end_pos;
            commit_type = L"soft_maxlen";
        } else if (words.size() >= 8) {
            for (int i = static_cast<int>(words.size()) - 1; i >= 0; --i) {
                if (words[i].ends_with_punctuation && i >= 7) {
                    cut_pos = words[i].end_pos;
                    commit_type = L"soft_semantic";
                    break;
                }
                if (words[i].is_conjunction && i >= 8) {
                    cut_pos = words[i].start_pos;
                    commit_type = L"soft_semantic";
                    break;
                }
            }
        }
        
        if (cut_pos != std::wstring::npos) {
            std::wstring chunk = remaining.substr(0, cut_pos);
            EmitTranslateCommit(commit_type, chunk, offset, duration, tsMs);
            g_transSegmenter.last_commit_pos += cut_pos;
        }
    }
    // --- END TRANSLATION SEGMENTATION LOGIC ---

    g_pktCount++;
    g_totalBytes  += pktBytes;
    g_lastDelayMs  = delayMs;
    FormatTimestamp(recvTick, g_lastTs, 20);

    // Extract new sentences via Splitter
    auto sentences = g_splitter.ExtractNewSentences(pktText, isFinal, offset);
    if (!sentences.empty()) {
        // Clear the current live line first to avoid character leftover
        ClearLiveText();

        for (const std::wstring& s : sentences) {
            ++g_splitter.sentence_idx;
            double offset_sec = static_cast<double>(offset) / 10000000.0;
            double duration_sec = static_cast<double>(duration) / 10000000.0;

            std::cout << "[COMMIT] " << g_splitter.sentence_idx << ". " << WideToUTF8(s) 
                      << " (offset: " << std::fixed << std::setprecision(2) << offset_sec << "s"
                      << ", duration: " << duration_sec << "s"
                      << ", id: " << WideToUTF8(resultId) << ")"
                      << std::endl;
        }

        // Output stats immediately after COMMIT
        const DWORD64 avg = (g_pktCount > 0) ? (g_totalBytes / g_pktCount) : 0;
        std::cout << "[STATS] Pkts: " << g_pktCount
                   << " | Bytes: " << g_totalBytes
                   << " | Avg: " << avg << " B"
                   << " | Delay: " << g_lastDelayMs << " ms"
                   << " | Last: " << WideToUTF8(g_lastTs)
                   << std::endl;
    }

    // Display remaining live text
    if (!isFinal) {
        std::wstring liveText = pktText;
        if (g_splitter.confirmed_len < liveText.size()) {
            liveText = liveText.substr(g_splitter.confirmed_len);
            size_t start = liveText.find_first_not_of(L' ');
            if (start != std::wstring::npos) {
                liveText = liveText.substr(start);
            }
        } else {
            liveText.clear();
        }
        PrintLiveText(liveText);
    } else {
        // If it is final, make sure to clean up the live line
        ClearLiveText();
    }
}

void CheckSilenceTimeoutLocked(DWORD64 now) {
    if (g_transSegmenter.last_packet_time > 0 && !g_transSegmenter.prev_text.empty()) {
        DWORD64 idle_time = now - g_transSegmenter.last_packet_time;
        DWORD64 threshold = g_transSegmenter.GetAdaptiveThreshold();
        
        if (idle_time > threshold) {
            std::wstring remaining = g_transSegmenter.prev_text.substr(g_transSegmenter.last_commit_pos);
            auto words = Tokenize(remaining);
            if (!words.empty()) {
                EmitTranslateCommit(L"soft_silence", remaining, g_transSegmenter.last_offset, 0, now);
                g_transSegmenter.last_commit_pos = g_transSegmenter.prev_text.size();
                g_transSegmenter.last_packet_time = 0; // Reset
            }
        }
    }
}

void CheckSilenceTimeout(DWORD64 now) {
    std::lock_guard<std::mutex> lock(g_csMutex);
    CheckSilenceTimeoutLocked(now);
}
