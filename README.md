# WinFeatures

A small native Windows program (plain C++ / Win32, no runtime or framework needed) that replaces
**Turn Windows features on or off** / **Optional features** on Windows 10/11 and
**Add Roles and Features** on Windows Server (local server).

> **Status:** compiled with a cross-compiler but not yet tested end-to-end on a real Windows machine.
> Try it on a test PC first. Anything that changes Windows features uses Windows' own servicing (DISM),
> the same engine the built-in tool uses.

## Install

1. Unzip the download.
2. Run **Setup.exe** (it asks for administrator rights).
3. Choose whether it should also open whenever Windows launches `optionalfeatures.exe`.

Silent install for scripts: `Setup.exe /S` (add `/replace` to take over optionalfeatures.exe).
Uninstall from *Settings > Apps*, or run `"%ProgramFiles%\WinFeatures\uninstall.exe" /uninstall` (add `/S` for silent).

You can also skip the installer and just run `WinFeatures.exe`.

## Digital signature and the icon

`Setup.exe`, `WinFeatures.exe` and `symfetch.exe` are signed with a **self-signed** certificate ("WinFeatures Self-Signed").
That proves the files haven't been altered since they were built, but Windows doesn't know the publisher, so SmartScreen or
UAC may still say *Unknown publisher*. That's expected; only a paid certificate from a trusted authority removes it.

- To make one PC trust the certificate, run `Trust-Certificate.bat` as administrator (it adds `WinFeatures.cer` to Trusted Root and Trusted Publishers;
  undo with `certutil -delstore Root "WinFeatures Self-Signed"`). Only do this on PCs you own, and only if you trust who built the download.
- To check a signature: right-click the file > Properties > Digital Signatures.
- If you rebuild, re-sign the new files (`signing\sign.bat`) or make your own certificate first with `signing\make-cert.ps1`.
  The private key file (`signing\WinFeatures-signing.pfx`) is not in the zip - keep it private.

The icon is `app.ico` (a bread roll with a green check), compiled into the program and the installer.

## Using it

- Tick a box to turn a feature on, untick to turn it off. A filled square means only some parts are on.
  Changed items turn bold; nothing happens until you press **Apply**.
- Hover over anything for a plain-English explanation. The **Suggestions** tab covers common goals
  (Linux, virtual machines, safer settings). The **Everything** tab shows all items in one list, colour-coded by type.
- **Ctrl +**, **Ctrl -**, **Ctrl 0** or **Ctrl + mouse wheel** make the text and boxes bigger or smaller.
- The **Rolls** tab changes the bread-roll background. (Yes, really.)
- On Windows Server it shows Roles and Features through the Server Manager cmdlets.

### When Windows can't find the files

If a feature fails because Windows Update didn't supply its files, the program explains it in plain English and:

- **Finds install media automatically** (a mounted ISO, a DVD/USB with `sources\sxs`, or a Features-on-Demand disc) and offers to retry.
- **Find...** next to *Source path* lets you pick media yourself: search drives, choose an ISO, or choose a folder.
- **Menu > Fix missing files automatically when a feature fails** (off by default) does this without asking:
  uses media if found, otherwise runs Windows' own repair (`DISM /Online /Cleanup-Image /RestoreHealth`) with a progress bar, then retries once.
- **Menu > Repair Windows automatically...** runs that repair on demand.

### Microsoft Update Catalog

**Menu > Get an update from Microsoft Update Catalog...** searches the catalog by KB number or name
(results are filtered to your PC's processor type), downloads with an inline progress bar, and - if
*Install automatically after download* is ticked - installs the package through DISM.

Safety rules built in: downloads are only accepted from Microsoft update servers, and auto-install only happens
if the file has a valid digital signature from *Microsoft Corporation* or *Microsoft Windows*; otherwise the file is deleted.
The catalog serves **update packages** (.msu/.cab), not the source files for optional features; for missing
feature files use the options above.

### Replacing the built-in app

**Menu > Use as the default Windows features app** sets an Image File Execution Options entry so
`optionalfeatures.exe` opens WinFeatures. The original Windows file is never modified. Undo it from
**Menu > Restore the original Windows app** or by uninstalling. Use **Update the installed default app** after building a new version.

## Settings

Stored per user in `HKCU\Software\WinFeatures` (beginner help, text size, auto-fix, etc.).

## Build from source

Needs a C++17 compiler: MSVC or MinGW-w64.

```
cd src
build.bat                       (MSVC: x64 Native Tools prompt)
```
MinGW:
```
windres app.rc -O coff -o app.o
g++ -std=c++17 -O2 -municode -mwindows -static main.cpp app.o -lcomctl32 -lshell32 -lole32 -lcomdlg32 -lwinhttp -lwintrust -lcrypt32 -o WinFeatures.exe
```
Installer: copy `WinFeatures.exe` into `installer\` and run `build-installer.bat`.
An older `rc.exe` may reject `/nologo`; the scripts don't use it.

Files: `app.ico` (icon), `main.cpp` (the app), `catalog.h` (portable Update Catalog parsing), `app.manifest` (asks for administrator, DPI aware),
`installer\setup.cpp` (the installer/uninstaller).

## Extra tool: symfetch

`tools\symfetch.exe` reads a Windows .exe/.dll and prints (or downloads) its Microsoft Symbol Server URL and PDB key:
`symfetch file.dll`, `--pdb`, `--download`, or `--id name timestamp sizeofimage`.
Symbol servers hold debug symbols, not feature files.

## Notes

- Requires administrator rights (the manifest asks for them).
- Not affiliated with Microsoft.
