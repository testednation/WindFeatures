@echo off
rem Optional. Makes this PC trust programs signed with the WinFeatures certificate (removes the "Unknown publisher" warning).
rem Run as administrator. Undo with:  certutil -delstore Root "WinFeatures Self-Signed"
net session >nul 2>&1 || (echo Please right-click this file and choose "Run as administrator". & pause & exit /b 1)
certutil -addstore -f Root "%~dp0WinFeatures.cer" && certutil -addstore -f TrustedPublisher "%~dp0WinFeatures.cer"
pause
