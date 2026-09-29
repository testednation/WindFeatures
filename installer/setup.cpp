// setup.cpp - tiny native installer/uninstaller for WinFeatures (no dependencies).
//   setup.exe                 interactive install
//   setup.exe /S [/replace]   silent install (/replace also replaces optionalfeatures.exe)
//   setup.exe /uninstall [/S] uninstall (this same file is copied to the install folder as uninstall.exe)
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <string>

static const wchar_t* kTitle = L"WinFeatures Setup";
static const wchar_t* kUninstKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\WinFeatures";
static bool g_silent = false;

static void Say(const std::wstring& t, UINT icon = MB_ICONINFORMATION) { if (!g_silent) MessageBoxW(nullptr, t.c_str(), kTitle, MB_OK | icon); }
static bool Ask(const std::wstring& t) { return g_silent || MessageBoxW(nullptr, t.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION) == IDYES; }

static std::wstring Known(int csidl) { wchar_t p[MAX_PATH] = {}; SHGetFolderPathW(nullptr, csidl, nullptr, 0, p); return p; }
static bool Has(const std::wstring& cl, const wchar_t* f) {
    std::wstring l = cl; for (auto& c : l) c = towlower(c);
    return l.find(f) != std::wstring::npos;
}

static bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, const std::wstring& desc) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return false;
    sl->SetPath(target.c_str()); sl->SetDescription(desc.c_str()); sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr; bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
    sl->Release();
    return ok;
}

static bool RunWait(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {}; std::wstring c = cmd;
    if (!CreateProcessW(nullptr, &c[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 60000);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return true;
}

static std::wstring InstallDir() { return Known(CSIDL_PROGRAM_FILES) + L"\\WinFeatures"; }
static std::wstring MenuDir()    { return Known(CSIDL_COMMON_PROGRAMS); }

static int Install(bool replace, bool askReplace) {
    std::wstring dir = InstallDir(), exe = dir + L"\\WinFeatures.exe", un = dir + L"\\uninstall.exe";
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(2), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    if (!g) { Say(L"The installer is damaged (program file missing).", MB_ICONERROR); return 1; }
    const void* data = LockResource(g); DWORD size = SizeofResource(nullptr, r);

    CreateDirectoryW(dir.c_str(), nullptr);
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w = 0;
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, data, size, &w, nullptr) || w != size) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        Say(L"Couldn't write " + exe + L".\r\nIf WinFeatures is open, close it and run Setup again.", MB_ICONERROR);
        return 1;
    }
    CloseHandle(f);

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    CopyFileW(self, un.c_str(), FALSE);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MakeShortcut(MenuDir() + L"\\Windows Features (WinFeatures).lnk", exe, L"Turn Windows features on or off");

    HKEY k;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        auto S = [&](const wchar_t* n, const std::wstring& v) { RegSetValueExW(k, n, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)); };
        auto D = [&](const wchar_t* n, DWORD v) { RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, 4); };
        S(L"DisplayName", L"WinFeatures (Windows features on or off)");
        S(L"DisplayVersion", L"1.0"); S(L"Publisher", L"WinFeatures");
        S(L"InstallLocation", dir); S(L"DisplayIcon", exe);
        S(L"UninstallString", L"\"" + un + L"\" /uninstall");
        S(L"QuietUninstallString", L"\"" + un + L"\" /uninstall /S");
        D(L"NoModify", 1); D(L"NoRepair", 1); D(L"EstimatedSize", (size + un.size()) / 1024 + 64);
        RegCloseKey(k);
    }

    if (askReplace) replace = Ask(L"Also make WinFeatures open whenever Windows launches \"Turn Windows features on or off\" "
                                  L"(Settings > More Windows features, Win+R optionalfeatures)?\r\n\r\n"
                                  L"The original Windows file is not modified, and you can undo this from the program's menu or by uninstalling.");
    if (replace) RunWait(L"\"" + exe + L"\" --install --silent");

    if (!g_silent && MessageBoxW(nullptr, L"Installed. Open WinFeatures now?", kTitle, MB_YESNO | MB_ICONINFORMATION) == IDYES)
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOW);
    return 0;
}

static int Uninstall() {
    std::wstring dir = InstallDir(), exe = dir + L"\\WinFeatures.exe";
    if (!Ask(L"Remove WinFeatures from this PC?\r\n(This also restores the original optionalfeatures.exe if it was replaced.)")) return 0;
    if (GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) RunWait(L"\"" + exe + L"\" --uninstall --silent");
    DeleteFileW((MenuDir() + L"\\Windows Features (WinFeatures).lnk").c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstKey);
    if (!DeleteFileW(exe.c_str()) && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Say(L"Couldn't delete WinFeatures.exe - it is probably still open. Close it and run the uninstaller again.", MB_ICONWARNING);
        return 1;
    }
    // This file is running, so let a helper delete it and the folder a moment after we exit.
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 >nul & del /f /q \"" + dir + L"\\uninstall.exe\" & rmdir \"" + dir + L"\"";
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    Say(L"WinFeatures has been removed.");
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring cl = GetCommandLineW();
    g_silent = Has(cl, L"/s") && !Has(cl, L"/setup");
    if (Has(cl, L"/uninstall")) return Uninstall();
    if (!g_silent && MessageBoxW(nullptr,
            L"This installs WinFeatures, a small replacement for \"Turn Windows features on or off\" and Server Manager's Roles and Features.\r\n\r\n"
            L"It will be installed to Program Files\\WinFeatures with a Start menu shortcut.\r\n\r\nContinue?",
            kTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
    return Install(Has(cl, L"/replace"), !g_silent && !Has(cl, L"/replace"));
}
