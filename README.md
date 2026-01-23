**Project Overview**
- **Description**: Native Windows injector + hook that extracts Live Captions text from the Microsoft Live Captions engine and renders a compact console UI while forwarding snapshots via a named pipe.
- **Components**: Loader (injector + console UI) and HookCore (DLL that hooks the caption-rendering function).

**How it works**
- **Injection**: Loader locates `LiveCaptions.exe` (or opens Live Captions settings if not running), then injects `HookCore.dll` into that process via `CreateRemoteThread` + `LoadLibraryW`.
- **Hooking**: HookCore waits for `microsoft.cognitiveservices.speech.extension.embedded.sr.runtime.dll`, locates the export `GetUnimicDecoderNBestDisplayText`, and installs a MinHook detour.
- **Extraction**: In the detour, the code reads a text pointer at offset `TEXT_OFFSET` (defined in dllmain.cpp) from the engine object, constructs a `std::wstring` snapshot, and sends it to the injector via the named pipe `\\.\pipe\LiveCaptionPipe`.
- **Rendering & Commit Engine**: Loader runs a pipe server, receives text snapshots, applies a "humane commit" algorithm (punctuation/length based) and renders active text + recent history in a simple console buffer.

**Security & Privacy**:
- Because this project hook into RAM when Live Captions is running and extract text from there, it is considered at a malware by Windows Defender and some antivirus software. You may need to whitelist the project folder or built binaries to avoid false positives.
- And for using in user mode, i have added code to set DACL permission for AppContainer read/execute on the DLL path before injection. But still, some antivirus may detect it as malware.
- If you do not trust the release, please build from source or check the SHA256 checksums in `SHA256SUMS.txt`.
cmd for checking SHA256:
```powershell
CertUtil -hashfile .\x64\Release\Loader.exe SHA256
CertUtil -hashfile .\x64\Release\HookCore.dll SHA256
```

**Build**
- **Open**: Open Native.sln in Visual Studio (target x64).
- **Note**: In my VS 2022 setup, I had to manually use the MinHook include/lib paths to project by editting the lib name from `libMinHook-x86-v141-mt.lib` to `libMinHook.lib`
- **Configuration**: Build x64 + `Release` (ensure both Loader and HookCore are built x64).
- **Output**: Built binaries appear under Release/.

**Run**
- Ensure Live Captions is enabled in Windows.
- Run the injector from the Release folder; if `LiveCaptions.exe` is not running the injector will open the Live Captions settings and wait.
- Example (PowerShell):
```powershell
cd .\x64\Release\
.\Loader.exe
```
- **Privileges**: If injection fails, run `Loader.exe` elevated (Administrator). Ensure bitness matches target process (x64 vs x86).

**Configuration & Tuning**
- **Text pointer offset**: Adjust `TEXT_OFFSET` in dllmain.cpp if the engine layout changes.
- **Target DLL / Function**: Modify the `targetDll` name or the export name `GetUnimicDecoderNBestDisplayText` in dllmain.cpp to match engine updates.
- **Pipe name**: Pipe is `\\.\pipe\LiveCaptionPipe` (both sides use this name).
