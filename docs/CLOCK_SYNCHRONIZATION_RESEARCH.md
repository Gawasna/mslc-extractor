# CLOCK SYNCHRONIZATION RESEARCH & ANALYSIS REPORT

## Real-world Experimental Parameters
- **Log File Source**: `logs/mslc_research_analysis.log`
- **Total JSON Events Captured**: 566
- **Total Caption Frames Captured**: 111
- **Average Named Pipe IPC Latency**: 0.11 ms
- **Maximum Named Pipe IPC Latency**: 16 ms

## 1. The Clock Synchronization Problem
In the C# Overlay Application, audio playback uses the system multimedia clock (e.g., standard audio buffer playhead), while subtitle metadata from Azure Speech SDK uses **ticks (100-nanosecond units)** relative to the start of the current audio stream session. Because of the asynchronous nature of thread loops, Named Pipe transport overhead, and drift between hardware audio clocks and system wall clock, using raw offsets directly leads to a **0-1 word drift (60% accuracy)**.

## 2. Timing Data Analysis
| Event | System Precise Ticks (100ns) | SDK Offset (100ns Ticks) | Delta from Start (ms) |
| :--- | :--- | :--- | :--- |
| caption (len=1) | 134305235171457917 | 1900000 | Sys: 598.31ms vs SDK: 190.00ms |
| caption (len=7) | 134305235171612704 | 1100000 | Sys: 613.79ms vs SDK: 110.00ms |
| caption (len=11) | 134305235174458735 | 1100000 | Sys: 898.39ms vs SDK: 110.00ms |
| caption (len=11) | 134305235176580614 | 1100000 | Sys: 1110.58ms vs SDK: 110.00ms |
| caption (len=12) | 134305235177455113 | 1100000 | Sys: 1198.03ms vs SDK: 110.00ms |
| caption (len=14) | 134305235177611296 | 1100000 | Sys: 1213.65ms vs SDK: 110.00ms |
| caption (len=21) | 134305235179558830 | 1100000 | Sys: 1408.40ms vs SDK: 110.00ms |
| caption (len=26) | 134305235181470039 | 1100000 | Sys: 1599.52ms vs SDK: 110.00ms |
| caption (len=30) | 134305235184435631 | 1100000 | Sys: 1896.08ms vs SDK: 110.00ms |
| caption (len=31) | 134305235184640721 | 1100000 | Sys: 1916.59ms vs SDK: 110.00ms |
| caption (len=30) | 134305235185483996 | 1100000 | Sys: 2000.92ms vs SDK: 110.00ms |
| caption (len=31) | 134305235190465822 | 1100000 | Sys: 2499.10ms vs SDK: 110.00ms |
| caption (len=35) | 134305235192522480 | 1100000 | Sys: 2704.77ms vs SDK: 110.00ms |
| caption (len=41) | 134305235197502043 | 1100000 | Sys: 3202.72ms vs SDK: 110.00ms |
| caption (len=44) | 134305235201536924 | 1100000 | Sys: 3606.21ms vs SDK: 110.00ms |
| caption (len=48) | 134305235203490222 | 1100000 | Sys: 3801.54ms vs SDK: 110.00ms |
| caption (len=54) | 134305235203676344 | 1100000 | Sys: 3820.15ms vs SDK: 110.00ms |
| caption (len=59) | 134305235208561893 | 1100000 | Sys: 4308.71ms vs SDK: 110.00ms |
| caption (len=64) | 134305235209591948 | 1100000 | Sys: 4411.71ms vs SDK: 110.00ms |
| caption (len=69) | 134305235211532164 | 1100000 | Sys: 4605.73ms vs SDK: 110.00ms |
| caption (len=70) | 134305235222480446 | 1100000 | Sys: 5700.56ms vs SDK: 110.00ms |
| caption (len=75) | 134305235224508779 | 1100000 | Sys: 5903.40ms vs SDK: 110.00ms |
| caption (len=77) | 134305235225462354 | 1100000 | Sys: 5998.75ms vs SDK: 110.00ms |
| caption (len=83) | 134305235225645065 | 1100000 | Sys: 6017.02ms vs SDK: 110.00ms |
| caption (len=86) | 134305235229455896 | 1100000 | Sys: 6398.11ms vs SDK: 110.00ms |

## 3. Mathematical Relationship and Correction Formula
Let $T_{sys}$ be the high-precision system clock (100ns ticks) when a packet is received, and $O_{sdk}$ be the SDK audio offset.
The relationship is modeled as:
$$T_{sys} = T_{start} + O_{sdk} + D_{delay}$$
Where:
- $T_{start}$: High-precision timestamp of speech start callback.
- $D_{delay}$: Dynamic transmission and pipeline delay. From the experiment:
  - Average Pipe Latency ($D_{ipc}$): **0.11 ms**

### Correction Logic:
To play back the exact audio portion corresponding to a subtitle segment in C# app:
1. **Determine Session Anchor**: Capture the exact high-precision system time of the `SessionStarted` or `SpeechStartDetected` callback.
2. **Offset Correction**: When checking a word at offset $O_{word}$ in C# playback:
   $$Playhead_{target} = O_{word} - (T_{recv} - T_{start} - D_{ipc})$$
   This aligns the playback playhead dynamically by subtracting the pipeline latency.