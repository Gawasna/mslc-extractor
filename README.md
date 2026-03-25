# Live Caption Extractor (MSLC Extractor)

**Project Overview**
- **Description**: Native Windows injector + hook that extracts Live Captions text from the Microsoft Live Captions engine by hooking the Azure Speech SDK core API and displaying real-time captions in a split-view console UI.
- **Components**: 
  - **Loader** (injector + split-view console UI with Named Pipe server)
  - **HookCore** (DLL that hooks Azure Speech SDK result APIs)

## How it Works

### Injection Process
- **Loader** locates `LiveCaptions.exe` (or opens Live Captions settings if not running)
- Sets DACL permissions on `HookCore.dll` for AppContainer sandbox compatibility
- Injects `HookCore.dll` into the target process via `CreateRemoteThread` + `LoadLibraryW`
- Starts Named Pipe server (`\\.\pipe\LiveCaptionPipe`) with NULL DACL for IPC

### Hooking Mechanism (Azure Speech SDK API)
- **HookCore** actively scans for `microsoft.cognitiveservices.speech.core.dll` using `EnumProcessModules`
- Locates two exports from Azure Speech SDK:
  - `result_get_text` - retrieves recognized text buffer
  - `result_get_reason` - queries recognition state (partial vs final)
- Installs MinHook detour on `result_get_text` to intercept all caption text
- Implements retry mechanism (20 attempts with 1-second intervals) for module detection

### Data Flow
1. **Capture**: Detour intercepts `result_get_text` calls with recognized text buffer (`char*`)
2. **State Detection**: Queries `result_get_reason` to determine if result is partial (`ResultReason_RecognizingSpeech`) or final (`ResultReason_RecognizedSpeech`)
3. **Logging**: Captured text is written to `C:\Users\Public\live_caption_debug.txt` with ISO 8601 timestamps
4. **Pipe Communication**: JSON payload sent via persistent Named Pipe connection to Loader
   - Format: `{"text":"...","is_final":true/false,"bytes":N,"ts_ms":T}`
   - Persistent connection (no reconnect per packet) to preserve splitter state
5. **Console Display**: Loader renders captions in split-view UI:
   - **Left panel**: Live stream with `[~]` (partial) and `[F]` (final) tags
   - **Center panel**: Confirmed sentences extracted via delta watermark splitter
   - **Right panel**: Real-time statistics (packet count, bytes, latency, timestamp)

### Key Implementation Details

**HookCore (DLL)**:
- **Hook Target**: `result_get_text` function (stdcall convention)
- **Function Signature**: `int __stdcall result_get_text(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen)`
- **Module Discovery**: Dynamic scanning via `FindModuleByPartialName()` instead of passive `GetModuleHandle()`
- **Text Format**: Narrow strings (`char*`) from core API, not wide strings from UI layer
- **Pipe Strategy**: Persistent connection with lazy reconnect on write failure (prevents splitter reset)
- **Thread Safety**: `CRITICAL_SECTION` guards pipe handle against concurrent hook calls

**Loader (Console UI)**:
- **Split-View Layout**: 3-column design with box-drawing characters (Unicode U+2502, U+2500, U+253C)
- **Sentence Splitter**: Delta watermark algorithm to prevent duplicate sentences
  - Tracks `confirmed_len` (chars already committed to Confirmed panel)
  - Scans only new suffix `[confirmed_len .. end]` for sentence boundaries (`.?!`)
  - Handles fast speech (multi-sentence batches) without duplication
  - Resets watermark on FINAL or regression detection
- **Logging**: Rolling window (100 lines max) written to `C:\Users\Public\loader_debug.txt`
- **Latency Tracking**: Measures pipe delay via `GetTickCount64()` delta (HookCore capture → Loader receive)

## Security & Privacy

⚠️ **Important Security Notice**:
- This project hooks into RAM when Live Captions is running and extracts text from there
- **Windows Defender and antivirus software will detect this as malware/PUP**
- You **must whitelist** the project folder or built binaries to avoid false positives
- For AppContainer compatibility, the code sets DACL permissions (read/execute) on the DLL before injection

**Trust & Verification**:
- If you do not trust the release, **build from source** or verify SHA256 checksums in `SHA256SUMS.txt`
- Verification commands:
```powershell
CertUtil -hashfile .\x64\Release\Loader.exe SHA256
CertUtil -hashfile .\x64\Release\HookCore.dll SHA256
```

## Build Instructions

### Prerequisites
- Visual Studio 2022 (or later) with C++ desktop development workload
- MinHook library (included in project)
- Target: x64 architecture

### Steps
1. **Open Solution**: Open `Native.sln` in Visual Studio
2. **Configure MinHook** (if needed):
   - In some VS 2022 setups, manually edit lib name from `libMinHook-x86-v141-mt.lib` to `libMinHook.lib`
   - Ensure MinHook include/lib paths are correctly set in project properties
3. **Build Configuration**: 
   - Set platform to `x64`
   - Set configuration to `Release`
   - Build both `Loader` and `HookCore` projects
4. **Output**: Built binaries will appear in `x64\Release\`

## Usage

### Prerequisites
- Windows 11 with Live Captions feature enabled
- Administrator privileges recommended (for injection)

### Running the Extractor

1. **Navigate to Release folder**:
```powershell
cd .\x64\Release\
```

2. **Run the Loader**:
```powershell
.\Loader.exe
```

3. **Expected Behavior**:
   - If `LiveCaptions.exe` is not running, Loader will open Live Captions settings and wait
   - Once detected, HookCore.dll will be injected
   - Console will display split-view UI:
     ```
     ┌─────────────────────────────────────────────┬─────────────────────────────────────────────┬──────────────────────────┐
     │              LIVE STREAM                    │         CONFIRMED SENTENCES                 │          STATS           │
     ├─────────────────────────────────────────────┼─────────────────────────────────────────────┼──────────────────────────┤
     │ [~] text being recognized...                │ 1. This is a complete sentence.             │ Pkts  : 42               │
     │ [F] completed sentence here.                │ 2. Another confirmed sentence.              │ Bytes : 1024             │
     │                                             │                                             │ Avg   : 24 B             │
     │                                             │                                             │ Delay : 15 ms            │
     │                                             │                                             │ Last  : 12:34:56         │
     └─────────────────────────────────────────────┴─────────────────────────────────────────────┴──────────────────────────┘
     ```
   - Debug logs:
     - HookCore: `C:\Users\Public\live_caption_debug.txt` (ISO 8601 timestamps)
     - Loader: `C:\Users\Public\loader_debug.txt` (rolling 100-line window)

### Troubleshooting

**Injection Fails**:
- Run `Loader.exe` as Administrator
- Ensure both Loader and target process are x64 (not x86)
- Check Windows Defender exclusions

**No Captions Captured**:
- Verify `live_caption_debug.txt` for diagnostic messages
- Check if Live Captions is actively transcribing audio
- Ensure correct DLL is being loaded (check log for "Core DLL handle found")
- Verify `result_get_text` and `result_get_reason` exports are resolved

**Module Not Found**:
- The DLL scan runs 20 times with 1-second intervals
- If still failing, Windows may have updated the core DLL name/structure
- Check log for "Could not find Core DLL handle after all retries"

**Duplicate Sentences**:
- Check `loader_debug.txt` for splitter watermark progression
- Verify splitter is not resetting mid-utterance (look for "REGRESSION detected")
- Ensure pipe connection is persistent (no "Pipe write failed" errors in HookCore log)

## Approach Justification

### HookCore Configuration (dllmain.cpp)

**Constants** (top of file):
- `LOG_PATH`: HookCore debug log location (default: `C:\Users\Public\live_caption_debug.txt`)
- `PIPE_NAME`: Named Pipe endpoint (default: `\\.\pipe\LiveCaptionPipe`)
- `MODULE_SCAN_RETRIES`: Max attempts to find core DLL (default: 20)
- `MODULE_SCAN_INTERVAL`: Delay between scans in ms (default: 1000)

**Target DLL**: `microsoft.cognitiveservices.speech.core.dll`
- Modify in `FindModuleByPartialName()` call if Microsoft changes the DLL name

**Target Functions**:
- `result_get_text` - hooked to intercept text buffer
- `result_get_reason` - called (not hooked) to query recognition state
- Update in `GetProcAddress()` calls if API signature changes

### Loader Configuration (Loader.cpp)

**Panel Layout** (top of file):
- `COL_LIVE_W`: Live stream panel width in chars (default: 46)
- `COL_CONFIRM_W`: Confirmed sentences panel width (default: 46)
- `COL_STATS_W`: Stats panel width (default: 26)
- `CONSOLE_H`: Console height in rows (default: 40)

**Sentence Splitter**:
- `BOUNDARIES`: Punctuation chars for sentence splitting (default: `.?!`)
- Modify in `SentenceSplitter::BOUNDARIES` if you want to include/exclude punctuation

## Architecture Evolution

### v1: UI Layer Hooking (Deprecated)
- Hooked `GetUnimicDecoderNBestDisplayText` from runtime DLL
- Extracted text from memory offset (`TEXT_OFFSET 0x190`)
- Wide string (`wchar_t*`) handling
- Fragile: broke on Windows updates

### v2: Core API Hooking (Current)
- Hooks `result_get_text` from Azure Speech SDK core DLL
- Queries `result_get_reason` for recognition state detection
- Direct API interception with standard function signature
- Narrow string (`char*`) handling
- Persistent Named Pipe connection to preserve splitter state
- Delta watermark sentence splitter to prevent duplicates

**Key Improvements**:
- Earlier access to recognized text (before UI rendering)
- Reliable partial/final detection via SDK API (not punctuation heuristics)
- Better compatibility across Windows updates
- Split-view console UI with real-time statistics
- Intelligent sentence splitting for fast speech (multi-sentence batches)
- Latency tracking (capture → display pipeline)

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Disclaimer

This tool is designed to enhance accessibility by extracting Live Captions output. Users are responsible for complying with local laws regarding audio recording and transcription.