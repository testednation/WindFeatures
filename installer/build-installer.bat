@echo off
rem Builds Setup.exe. First copy the finished WinFeatures.exe into this folder.
rem Run from an "x64 Native Tools Command Prompt for VS".
rc setup.rc || exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /MT /DUNICODE /D_UNICODE setup.cpp setup.res /Fe:Setup.exe /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup ole32.lib shell32.lib uuid.lib
rem MinGW alternative:
rem   windres setup.rc -O coff -o setup_res.o
rem   g++ -std=c++17 -O2 -municode -mwindows -static setup.cpp setup_res.o -lole32 -lshell32 -luuid -o Setup.exe
