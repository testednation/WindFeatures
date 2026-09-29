@echo off
rem Run from a "x64 Native Tools Command Prompt for VS" (MSVC).
rc app.rc || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /DUNICODE /D_UNICODE main.cpp app.res /Fe:WinFeatures.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup
rem MinGW alternative:
rem   windres app.rc -O coff -o app.o
rem   g++ -std=c++17 -O2 -municode -mwindows -static main.cpp app.o -lcomctl32 -lshell32 -lole32 -lcomdlg32 -lwinhttp -lwintrust -lcrypt32 -o WinFeatures.exe
