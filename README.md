# Live Caption Extractor (MSLC Extractor)

**Project Overview**
- **Description**: Native Windows injector + hook that extracts Live Captions text from the Microsoft Live Captions engine by hooking the core speech recognition API and logging output to file with optional real-time console display.
- **Components**: 
  - **Loader** (injector + console UI with Named Pipe listener)
  - **HookCore** (DLL that hooks the core speech recognition function)

## How it Works

### Injection Process
- **Loader** locates `LiveCaptions.exe` (or opens Live Captions settings if not running)
- Injects `HookCore.dll` into the target process via `CreateRemoteThread` + `LoadLibraryW`
- Sets proper DACL permissions for AppContainer execution before injection

### Hooking Mechanism (Core API Approach)
- **HookCore** actively scans for `microsoft.cognitiveservices.speech.core.dll` using `EnumProcessModules`
- Locates the export `result_get_text` (core speech recognition API)
- Installs a MinHook detour to intercept caption text at the API level
- Implements retry mechanism (20 attempts with 1-second intervals) for module detection

### Data Flow
1. **Capture**: Detour intercepts `result_get_text` calls with recognized text buffer (`char*`)
2. **Logging**: Captured text is written to `C:\Users\Public\live_caption_debug.txt` with timestamps
3. **Pipe Communication** (Optional): Text can be sent via Named Pipe `\\.\pipe\LiveCaptionPipe` to Loader
4. **Console Display**: Loader renders real-time captions with `[Partial]` and `[FINAL]` tags

### Key Implementation Details
- **Hook Target**: `result_get_text` function (stdcall convention)
- **Function Signature**: `int __stdcall result_get_text(SPXRESULTHANDLE hresult, char* buffer, uint32_t bufferLen)`
- **Module Discovery**: Dynamic scanning via `FindModuleByPartialName()` instead of passive `GetModuleHandle()`
- **Text Format**: Narrow strings (`char*`) from core API, not wide strings from UI layer

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
   - Console will display real-time captions:
     ```
     [Partial]: text being recognized...
     [FINAL]: completed sentence here.
     ```
   - Debug log will be written to `C:\Users\Public\live_caption_debug.txt`

### Troubleshooting

**Injection Fails**:
- Run `Loader.exe` as Administrator
- Ensure both Loader and target process are x64 (not x86)
- Check Windows Defender exclusions

**No Captions Captured**:
- Verify `live_caption_debug.txt` for diagnostic messages
- Check if Live Captions is actively transcribing audio
- Ensure correct DLL is being loaded (check log for "SUCCESS: Core DLL Handle found")

**Module Not Found**:
- The DLL scan runs 20 times with 1-second intervals
- If still failing, Windows may have updated the core DLL name/structure

## Approach Justification

### HookCore Settings (dllmain.cpp)

- **Target DLL**: `microsoft.cognitiveservices.speech.core.dll`
  - Modify in `FindModuleByPartialName()` call if Microsoft changes the DLL name
  
- **Target Function**: `result_get_text`
  - Update in `GetProcAddress()` call if API signature changes

## Architecture Changes (v33fd07c)

This version represents a **major architectural shift**:

**Previous Approach** (UI Hooking):
- Hooked `GetUnimicDecoderNBestDisplayText` from runtime DLL
- Extracted text from memory offset (`TEXT_OFFSET 0x190`)
- Wide string (`wchar_t*`) handling

**Current Approach** (Core API Hooking):
- Hooks `result_get_text` from core speech DLL
- Direct API interception with standard function signature
- Narrow string (`char*`) handling
- More reliable and update-resistant

**Benefits**:
- Earlier access to recognized text (before UI rendering)
- Better compatibility across Windows updates
- Cleaner codebase with file-based debugging
- More robust module detection

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Disclaimer

This tool is designed to enhance accessibility by extracting Live Captions output. Users are responsible for complying with local laws regarding audio recording and transcription.