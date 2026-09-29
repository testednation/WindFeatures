// WinFeatures - native C++ replacement for:
//   * "Turn Windows features on or off"  (optionalfeatures.exe, Windows client)
//   * Settings > Optional features       (Features on Demand / capabilities)
//   * Server Manager > Add/Remove Roles and Features (local server)
//
// Dependencies: none beyond Windows itself.
//   - Windows client : DISM API (dismapi.dll, loaded dynamically)
//   - Windows Server : ServerManager PowerShell cmdlets (Get/Install/Uninstall-WindowsFeature)
//                      for roles & features, DISM API for optional features
//
// Replacing optionalfeatures.exe: File > "Use as default..." registers this program
// through Image File Execution Options (reversible, the system file is not modified).
// CLI: --install | --uninstall | --silent | --server | --client
//
// Must run elevated (manifest requests administrator).

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define WSTR2(x) L##x
#define WSTR(x) WSTR2(x)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <cwchar>
#include <shlobj.h>
#include <commdlg.h>
#include <shellapi.h>
#include <winhttp.h>
#include <wintrust.h>
#include <wincrypt.h>
#include <functional>
#include "catalog.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#ifndef TVS_EX_DOUBLEBUFFER
#define TVS_EX_DOUBLEBUFFER 0x0004
#endif
#ifndef TVS_EX_AUTOHSCROLL
#define TVS_EX_AUTOHSCROLL 0x0020
#endif
#ifndef TVS_EX_FADEINOUTEXPANDOS
#define TVS_EX_FADEINOUTEXPANDOS 0x0040
#endif
#ifndef TVS_EX_PARTIALCHECKBOX
#define TVS_EX_PARTIALCHECKBOX 0x0080
#endif

// ---------------------------------------------------------------------------
// Minimal DISM API declarations (mirrors dismapi.h; structs are pack(4)).
// ---------------------------------------------------------------------------
namespace dism {

#pragma pack(push, 4)
struct Feature        { PCWSTR Name; UINT State; };
struct FeatureInfo    { PCWSTR Name; UINT State; PCWSTR DisplayName; PCWSTR Description;
                        UINT RestartRequired; void* CustomProperty; UINT CustomPropertyCount; };
struct Capability     { PCWSTR Name; UINT State; };
struct CapabilityInfo { PCWSTR Name; UINT State; PCWSTR DisplayName; PCWSTR Description;
                        DWORD DownloadSize; DWORD InstallSize; };
struct String         { PCWSTR Value; };
#pragma pack(pop)

enum State { NotPresent = 0, UninstallPending = 1, Staged = 2, Installed = 3,
             InstallPending = 4, Superseded = 5, PartiallyInstalled = 6 };

typedef void (CALLBACK* ProgressCb)(UINT, UINT, PVOID);
typedef UINT Session;

static const wchar_t* kOnlineImage = L"DISM_{53BFAE52-B167-4E2F-A258-0A37B57FF845}";
static const HRESULT kRebootRequired = HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED);

struct Api {
    HMODULE mod = nullptr;
    HRESULT (WINAPI *Initialize)(int, PCWSTR, PCWSTR) = nullptr;
    HRESULT (WINAPI *Shutdown)() = nullptr;
    HRESULT (WINAPI *OpenSession)(PCWSTR, PCWSTR, PCWSTR, Session*) = nullptr;
    HRESULT (WINAPI *CloseSession)(Session) = nullptr;
    HRESULT (WINAPI *GetFeatures)(Session, PCWSTR, int, Feature**, UINT*) = nullptr;
    HRESULT (WINAPI *GetFeatureInfo)(Session, PCWSTR, PCWSTR, int, FeatureInfo**) = nullptr;
    HRESULT (WINAPI *EnableFeature)(Session, PCWSTR, PCWSTR, int, BOOL, PCWSTR*, UINT, BOOL, HANDLE, ProgressCb, PVOID) = nullptr;
    HRESULT (WINAPI *DisableFeature)(Session, PCWSTR, PCWSTR, BOOL, HANDLE, ProgressCb, PVOID) = nullptr;
    HRESULT (WINAPI *GetCapabilities)(Session, Capability**, UINT*) = nullptr;
    HRESULT (WINAPI *GetCapabilityInfo)(Session, PCWSTR, CapabilityInfo**) = nullptr;
    HRESULT (WINAPI *AddCapability)(Session, PCWSTR, BOOL, PCWSTR*, UINT, HANDLE, ProgressCb, PVOID) = nullptr;
    HRESULT (WINAPI *RemoveCapability)(Session, PCWSTR, HANDLE, ProgressCb, PVOID) = nullptr;
    HRESULT (WINAPI *AddPackage)(Session, PCWSTR, BOOL, BOOL, HANDLE, ProgressCb, PVOID) = nullptr;
    HRESULT (WINAPI *Delete)(VOID*) = nullptr;
    HRESULT (WINAPI *GetLastErrorMessage)(String**) = nullptr;

    bool Load() {
        mod = LoadLibraryExW(L"dismapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!mod) return false;
        #define BIND(f) f = reinterpret_cast<decltype(f)>(GetProcAddress(mod, "Dism" #f)); if (!f) return false;
        BIND(Initialize) BIND(Shutdown) BIND(OpenSession) BIND(CloseSession)
        BIND(GetFeatures) BIND(GetFeatureInfo) BIND(EnableFeature) BIND(DisableFeature)
        BIND(GetCapabilities) BIND(GetCapabilityInfo) BIND(AddCapability) BIND(RemoveCapability)
        BIND(AddPackage) BIND(Delete) BIND(GetLastErrorMessage)
        #undef BIND
        return true;
    }
};

} // namespace dism

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
enum { WM_LOADED = WM_APP + 1, WM_PROGRESS, WM_APPLIED, WM_STATUSMSG, WM_TOGGLE,
       WM_REPAIRED, WM_CAT_STATUS, WM_CAT_PROG, WM_CAT_RESULTS, WM_CAT_DONE };
enum { ID_TAB = 100, ID_SEARCH, ID_FILTER, ID_EXPAND, ID_COLLAPSE, ID_TREE, ID_DTITLE, ID_DESC,
       ID_SRCLBL, ID_SRC, ID_BROWSE, ID_PAYLOAD, ID_PROG, ID_STATUS, ID_APPLY, ID_REFRESH, ID_MENUBTN };
enum { IDM_SETDEFAULT = 201, IDM_RESTORE, IDM_EXIT, IDM_EXPAND = 210, IDM_COLLAPSE, IDM_REFRESH, IDM_ABOUT = 220,
       IDM_BEGINNER = 233, IDM_GUIDE, IDM_TEXT_100 = 240, IDM_TEXT_125, IDM_TEXT_150,
       IDM_CATALOG = 250, IDM_REPAIR, IDM_AUTOFIX };

enum Kind { K_DISM_FEATURE = 0, K_DISM_CAPABILITY = 1, K_SERVER_FEATURE = 2, K_ROLL = 3 };

struct Node {
    std::wstring name, display, desc, type;
    UINT state = 0;
    bool orig = false, want = false;
    bool payloadRemoved = false;
    int  kind = K_DISM_FEATURE;
    int  tab = 0;
    int  parent = -1;
    int  depth = 0;
    UINT restart = 0;
    DWORD dlSize = 0, instSize = 0;
    std::vector<int> kids;
    HTREEITEM h = nullptr;
};

struct LoadResult {
    std::vector<Node> nodes;
    std::vector<std::wstring> tabs;
    HRESULT hr = S_OK;
    std::wstring err;
};

struct Job { int kind; bool enable; int depth; std::wstring name; };
struct ApplyArgs { std::vector<Job> jobs; std::wstring source; bool removePayload = false; };

struct ApplyResult {
    int ok = 0, failed = 0;
    bool reboot = false;
    bool sourceProblem = false;                              // Windows could not find/download the files
    std::vector<std::pair<std::wstring, bool>> fails;        // (name, wanted-on) of items that failed
    std::wstring errors;
};
struct RetryItem { std::wstring name; bool enable; };
static std::vector<RetryItem> g_retry;
static bool g_retryPending = false;

static dism::Api g_api;
static bool      g_initialized = false;
static dism::Session g_session = 0;
static bool      g_haveSession = false;
static bool      g_isServer = false;

static HWND g_wnd, g_tab, g_search, g_filter, g_expand, g_collapse, g_tree, g_dtitle, g_desc,
            g_srcLbl, g_src, g_browse, g_payload, g_prog, g_status, g_apply, g_refresh;
static HFONT g_font, g_fontBold;
static int   g_dpi = 96;
static std::vector<Node> g_nodes;
static int   g_cur = 0;
static bool  g_busy = false;

// appearance / help state (light theme only)
static bool g_beginner = true;
static HWND g_menuBtn, g_tt;
static HBRUSH g_brBg, g_brCtl;
static COLORREF g_cBg, g_cCtl, g_cText, g_cSub, g_cAccent, g_cLine;
static int  g_sugTab = -1;
static int  g_allTab = -1;
static int  g_textPct = 100;      // text/control size: 100, 125 or 150

static int S(int v) { return MulDiv(v, g_dpi * g_textPct / 100, 96); }   // layout units (DPI x text size)
static int SD(int v) { return MulDiv(v, g_dpi, 96); }                     // DPI only
static DWORD RegGetU(const wchar_t* n, DWORD d);
static void RegPutU(const wchar_t* n, DWORD v);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::wstring DismError(HRESULT hr) {
    std::wstring out;
    dism::String* s = nullptr;
    if (g_api.GetLastErrorMessage && SUCCEEDED(g_api.GetLastErrorMessage(&s)) && s) {
        if (s->Value) out = s->Value;
        g_api.Delete(s);
    }
    if (out.empty()) {
        wchar_t* sys = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, (DWORD)hr, 0, (LPWSTR)&sys, 0, nullptr);
        if (sys) { out = sys; LocalFree(sys); }
    }
    while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    wchar_t code[32];
    wsprintfW(code, L" (0x%08X)", (unsigned)hr);
    return out + code;
}

// Plain-English explanations for the errors people actually hit.
static const wchar_t* PlainError(HRESULT hr) {
    switch ((unsigned)hr) {
    case 0x800F081F: return L"Windows couldn't find the files for this feature. They aren't on this PC and Windows Update didn't supply them. Use install media (Find...) to provide them.";
    case 0x800F0906: return L"Windows couldn't download the files it needs. Check your internet connection, or use install media (Find...).";
    case 0x800F0907: return L"A policy on this PC blocks Windows from downloading feature files. Ask your administrator, or use install media (Find...).";
    case 0x800F0954: return L"This PC gets its updates from a company update server (WSUS) that doesn't offer feature files. Ask your administrator to allow direct downloads, or use install media (Find...).";
    case 0x800F080C: return L"Windows doesn't recognise this feature. It may not exist in this edition or version of Windows.";
    case 0x800F0922: return L"Windows couldn't finish installing. This often means a restart is pending or the system partition is full. Restart the PC and try again.";
    case 0x800F0831: return L"Windows' component store is missing a package. Run 'DISM /Online /Cleanup-Image /RestoreHealth' and try again.";
    case 0x80073712: return L"Windows' component store is damaged. Run 'DISM /Online /Cleanup-Image /RestoreHealth', then try again.";
    case 0x80070422: return L"The Windows Update service is turned off. Turn it on in Services and try again.";
    case 0x80072EE7: case 0x80072EFD: case 0x80072EE2:
        return L"Windows couldn't reach the internet. Check your connection or proxy, or use install media (Find...).";
    case 0x8024402C: case 0x80244022:
        return L"Windows Update couldn't be reached. Check your connection or proxy, or use install media (Find...).";
    case 0x80070005: return L"Access denied. Make sure the app is running as administrator.";
    case 0x80070070: return L"There isn't enough free disk space.";
    case 0x80070002: case 0x80070003: return L"A file or folder wasn't found. Check the Source path.";
    case 0x80070032: return L"This isn't supported on this edition of Windows or on this hardware.";
    }
    return nullptr;
}
static bool IsSourceProblem(HRESULT hr) {
    unsigned u = (unsigned)hr;
    return u == 0x800F081F || u == 0x800F0906 || u == 0x800F0907 || u == 0x800F0954;
}

static bool IsOn(UINT st) { return st == dism::Installed || st == dism::InstallPending || st == dism::PartiallyInstalled; }

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
    size_t off = (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) ? 3 : 0;
    if (s.size() <= off) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data() + off, (int)(s.size() - off), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data() + off, (int)(s.size() - off), &w[0], n);
    return w;
}

static void SetStatus(const std::wstring& t) { SetWindowTextW(g_status, t.c_str()); }
static void PostStatus(const std::wstring& t) { PostMessageW(g_wnd, WM_STATUSMSG, 0, (LPARAM) new std::wstring(t)); }

static bool DetectServer() {
    wchar_t v[32] = {};
    DWORD sz = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\ProductOptions", L"ProductType",
                     RRF_RT_REG_SZ, nullptr, v, &sz) == ERROR_SUCCESS)
        return _wcsicmp(v, L"WinNT") != 0;
    return false;
}

// Runs a command line, captures stdout (UTF-8). Returns false if the process could not start.
static DWORD g_lastExit = 0;
static bool RunCapture(const std::wstring& cmd, std::string& out, void (*chunk)(const char*, DWORD, void*) = nullptr, void* ud = nullptr) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = nul; si.hStdInput = nul;
    PROCESS_INFORMATION pi = {};
    std::wstring c = cmd;
    BOOL ok = CreateProcessW(nullptr, &c[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(rd); return false; }
    char buf[8192]; DWORD n;
    while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) { out.append(buf, n); if (chunk) chunk(buf, n, ud); }
    WaitForSingleObject(pi.hProcess, INFINITE);
    g_lastExit = 1; GetExitCodeProcess(pi.hProcess, &g_lastExit);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return true;
}

static std::wstring PowerShellCmd(const std::wstring& script) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive "
           L"-ExecutionPolicy Bypass -Command \"[Console]::OutputEncoding=[Text.Encoding]::UTF8; " + script + L"\"";
}

static std::vector<std::vector<std::wstring>> ParseCsv(const std::wstring& s) {
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring f;
    bool q = false;
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (q) {
            if (c == L'"') { if (i + 1 < s.size() && s[i + 1] == L'"') { f += L'"'; ++i; } else q = false; }
            else f += c;
        } else if (c == L'"') q = true;
        else if (c == L',') { row.push_back(f); f.clear(); }
        else if (c == L'\r') {}
        else if (c == L'\n') {
            row.push_back(f); f.clear();
            if (!(row.size() == 1 && row[0].empty())) rows.push_back(row);
            row.clear();
        } else f += c;
    }
    if (!f.empty() || !row.empty()) { row.push_back(f); rows.push_back(row); }
    return rows;
}

static bool SafeName(const std::wstring& n) {
    if (n.empty()) return false;
    for (wchar_t c : n) if (!(iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'~')) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Model: tree building / tri-state logic
// ---------------------------------------------------------------------------
static void SumKids(const std::vector<Node>& v, int i, int& on, int& total) {
    for (int k : v[i].kids) { ++total; if (v[k].want) ++on; SumKids(v, k, on, total); }
}

// 1 = unchecked, 2 = checked, 3 = partial
static int Disp(int i) {
    int on = 0, total = 0;
    SumKids(g_nodes, i, on, total);
    if (g_nodes[i].want) return on == total ? 2 : 3;
    return on > 0 ? 3 : 1;
}

static void SetSubtree(int i, bool v) {
    g_nodes[i].want = v;
    for (int k : g_nodes[i].kids) SetSubtree(k, v);
}

static int PendingCount() {
    int n = 0;
    for (auto& x : g_nodes) if (x.want != x.orig) ++n;
    return n;
}

static void BuildHierarchy(std::vector<Node>& nodes) {
    // Server nodes already carry parent indexes. For DISM features use name-prefix heuristic.
    for (size_t i = 0; i < nodes.size(); ++i) {
        Node& n = nodes[i];
        if (n.kind != K_DISM_FEATURE || n.parent != -1) continue;
        std::wstring ln = Lower(n.name);
        size_t best = 0; int bi = -1;
        for (size_t j = 0; j < nodes.size(); ++j) {
            if (j == i || nodes[j].kind != K_DISM_FEATURE || nodes[j].tab != n.tab) continue;
            std::wstring lp = Lower(nodes[j].name);
            if (lp.size() < ln.size() && ln.compare(0, lp.size(), lp) == 0 && ln[lp.size()] == L'-' && lp.size() > best) {
                best = lp.size(); bi = (int)j;
            }
        }
        n.parent = bi;
    }
    for (auto& n : nodes) n.kids.clear();
    for (size_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].parent >= 0) nodes[nodes[i].parent].kids.push_back((int)i);
    for (size_t i = 0; i < nodes.size(); ++i) {
        int d = 0, p = nodes[i].parent;
        while (p >= 0 && d < 64) { ++d; p = nodes[p].parent; }
        nodes[i].depth = d;
        auto& k = nodes[i].kids;
        std::sort(k.begin(), k.end(), [&](int a, int b) {
            return _wcsicmp((nodes[a].display.empty() ? nodes[a].name : nodes[a].display).c_str(),
                            (nodes[b].display.empty() ? nodes[b].name : nodes[b].display).c_str()) < 0;
        });
    }
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------
static bool LoadServerFeatures(std::vector<Node>& out) {
    std::string raw;
    if (!RunCapture(PowerShellCmd(L"Get-WindowsFeature | Select-Object Name,DisplayName,Description,FeatureType,Path,InstallState "
                                  L"| ConvertTo-Csv -NoTypeInformation"), raw)) return false;
    auto rows = ParseCsv(Utf8ToWide(raw));
    if (rows.size() < 2) return false;
    int cN = -1, cD = -1, cDe = -1, cT = -1, cP = -1, cS = -1;
    for (size_t c = 0; c < rows[0].size(); ++c) {
        const std::wstring& h = rows[0][c];
        if (h == L"Name") cN = (int)c; else if (h == L"DisplayName") cD = (int)c; else if (h == L"Description") cDe = (int)c;
        else if (h == L"FeatureType") cT = (int)c; else if (h == L"Path") cP = (int)c; else if (h == L"InstallState") cS = (int)c;
    }
    if (cN < 0 || cS < 0) return false;
    std::vector<std::wstring> paths;
    auto get = [](const std::vector<std::wstring>& r, int c) { return (c >= 0 && c < (int)r.size()) ? r[c] : std::wstring(); };
    for (size_t r = 1; r < rows.size(); ++r) {
        Node n;
        n.kind = K_SERVER_FEATURE;
        n.name = get(rows[r], cN);
        if (!SafeName(n.name)) continue;
        n.display = get(rows[r], cD);
        n.desc = get(rows[r], cDe);
        n.type = get(rows[r], cT);
        std::wstring st = get(rows[r], cS);
        n.payloadRemoved = (st == L"Removed");
        n.state = st == L"Installed" ? dism::Installed : st == L"InstallPending" ? dism::InstallPending
                : st == L"UninstallPending" ? dism::UninstallPending : dism::Staged;
        n.orig = n.want = IsOn(n.state);
        n.tab = (n.type == L"Role" || n.type == L"Role Service") ? 0 : 1;
        out.push_back(n);
        paths.push_back(get(rows[r], cP));
    }
    // parents from Path ("Root\Child\Grandchild")
    for (size_t i = 0; i < out.size(); ++i) {
        const std::wstring& p = paths[i];
        size_t last = p.rfind(L'\\');
        if (last == std::wstring::npos || last == 0) continue;
        size_t prev = p.rfind(L'\\', last - 1);
        size_t start = (prev == std::wstring::npos) ? 0 : prev + 1;
        std::wstring parent = p.substr(start, last - start);
        for (size_t j = 0; j < out.size(); ++j)
            if (j != i && _wcsicmp(out[j].name.c_str(), parent.c_str()) == 0 && out[j].tab == out[i].tab) { out[i].parent = (int)j; break; }
    }
    return !out.empty();
}

// A little fun: bread "Rolls" alongside Roles and Features. These only store a tick mark in this
// app's own settings (HKCU\Software\WinFeatures) and never change Windows.
static void AddRolls(std::vector<Node>& out, int tab) {
    struct R { const wchar_t* id; const wchar_t* title; const wchar_t* desc; int parent; int level; };
    static const R rolls[] = {
        { L"Roll-Bakery", L"Bakery", L"The main job of any good bread basket. Turn it on to unlock the rolls below.", -1, 0 },
        { L"Roll-Soft", L"Soft Rolls", L"Pillowy, gently sweet rolls.", 0, 1 },
        { L"Roll-Dinner", L"Dinner Roll", L"The classic. Small, soft and perfect with butter.", 1, 2 },
        { L"Roll-Brioche", L"Brioche Roll", L"Rich with eggs and butter. Makes any burger feel fancy.", 1, 2 },
        { L"Roll-Hawaiian", L"Hawaiian Sweet Roll", L"Sweet, fluffy and great for sliders.", 1, 2 },
        { L"Roll-ParkerHouse", L"Parker House Roll", L"Folded, buttery and best served warm.", 1, 2 },
        { L"Roll-Crusty", L"Crusty Rolls", L"Crisp outside, chewy inside.", 0, 1 },
        { L"Roll-Kaiser", L"Kaiser Roll", L"A crisp roll with a pinwheel top. Sandwich royalty.", 6, 2 },
        { L"Roll-Ciabatta", L"Ciabatta Roll", L"Open, chewy crumb and a floury crust. Ideal for panini.", 6, 2 },
        { L"Roll-PapoSeco", L"Papo Seco", L"The Portuguese 'dry roll' with a crackly crust and a soft middle.", 6, 2 },
        { L"Roll-PetitPain", L"Petit Pain", L"A small French roll, basically a baguette that fits in your hand.", 6, 2 },
        { L"Roll-Pretzel", L"Pretzel Roll", L"Dark, glossy and salty with a chewy bite.", 0, 1 },
        { L"Roll-Toppings", L"Toppings", L"Finishing touches.", 0, 1 },
        { L"Roll-Butter", L"Butter", L"Non-negotiable.", 12, 2 },
        { L"Roll-Sesame", L"Sesame Seeds", L"A nutty crunch on top.", 12, 2 },
        { L"Roll-Poppy", L"Poppy Seeds", L"Tiny, crunchy and famously stuck in teeth.", 12, 2 },
        { L"Roll-Jam", L"Jam", L"For the sweet-toothed.", 12, 2 },
    };
    int base = (int)out.size();
    for (const R& x : rolls) {
        Node n;
        n.kind = K_ROLL; n.name = x.id; n.display = x.title; n.desc = x.desc;
        n.type = x.level == 0 ? L"Role" : x.level == 1 ? L"Role Service" : L"Feature";
        n.tab = tab;
        n.parent = x.parent < 0 ? -1 : base + x.parent;
        n.orig = n.want = RegGetU(x.id, 0) != 0;
        n.state = n.orig ? dism::Installed : dism::Staged;
        out.push_back(n);
    }
}

static HRESULT LoadImpl(LoadResult& r) {
    if (!g_initialized) {
        if (!g_api.Load()) { r.err = L"dismapi.dll could not be loaded."; return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND); }
        HRESULT hr = g_api.Initialize(0, nullptr, nullptr);
        if (FAILED(hr)) { r.err = DismError(hr); return hr; }
        g_initialized = true;
    }
    if (g_haveSession) { g_api.CloseSession(g_session); g_haveSession = false; }
    HRESULT hr = g_api.OpenSession(dism::kOnlineImage, nullptr, nullptr, &g_session);
    if (FAILED(hr)) { r.err = DismError(hr); return hr; }
    g_haveSession = true;

    bool serverOk = false;
    if (g_isServer) {
        PostStatus(L"Reading roles and features...");
        serverOk = LoadServerFeatures(r.nodes);
        if (!serverOk) r.nodes.clear();
    }
    int capTab = serverOk ? 2 : 1;
    if (serverOk) r.tabs = { L"Roles", L"Features", L"Optional features" };
    else          r.tabs = { L"Windows features", L"Optional features" };

    if (!serverOk) {
        dism::Feature* f = nullptr; UINT n = 0;
        PostStatus(L"Reading Windows features...");
        hr = g_api.GetFeatures(g_session, nullptr, 0, &f, &n);
        if (SUCCEEDED(hr)) {
            for (UINT i = 0; i < n; ++i) {
                if (f[i].State == dism::NotPresent) continue;
                Node nd;
                nd.kind = K_DISM_FEATURE; nd.name = f[i].Name; nd.state = f[i].State;
                nd.orig = nd.want = IsOn(f[i].State); nd.tab = 0;
                dism::FeatureInfo* fi = nullptr;
                if (SUCCEEDED(g_api.GetFeatureInfo(g_session, f[i].Name, nullptr, 0, &fi)) && fi) {
                    if (fi->DisplayName) nd.display = fi->DisplayName;
                    if (fi->Description) nd.desc = fi->Description;
                    nd.restart = fi->RestartRequired;
                    g_api.Delete(fi);
                }
                r.nodes.push_back(std::move(nd));
                if (i % 25 == 0) PostStatus(L"Reading Windows features... " + std::to_wstring(i) + L"/" + std::to_wstring(n));
            }
            g_api.Delete(f);
        } else r.err = DismError(hr);
    }

    dism::Capability* c = nullptr; UINT n = 0;
    PostStatus(L"Reading optional features...");
    HRESULT hc = g_api.GetCapabilities(g_session, &c, &n);
    if (SUCCEEDED(hc)) {
        for (UINT i = 0; i < n; ++i) {
            if (c[i].State == dism::NotPresent) continue;
            Node nd;
            nd.kind = K_DISM_CAPABILITY; nd.name = c[i].Name; nd.state = c[i].State;
            nd.orig = nd.want = IsOn(c[i].State); nd.tab = capTab;
            dism::CapabilityInfo* ci = nullptr;
            if (SUCCEEDED(g_api.GetCapabilityInfo(g_session, c[i].Name, &ci)) && ci) {
                if (ci->DisplayName) nd.display = ci->DisplayName;
                if (ci->Description) nd.desc = ci->Description;
                nd.dlSize = ci->DownloadSize; nd.instSize = ci->InstallSize;
                g_api.Delete(ci);
            }
            r.nodes.push_back(std::move(nd));
            if (i % 25 == 0) PostStatus(L"Reading optional features... " + std::to_wstring(i) + L"/" + std::to_wstring(n));
        }
        g_api.Delete(c);
    } else if (r.err.empty()) r.err = DismError(hc);

    r.tabs.push_back(L"Rolls");
    AddRolls(r.nodes, (int)r.tabs.size() - 1);
    BuildHierarchy(r.nodes);
    return S_OK;
}

static DWORD WINAPI LoadThread(LPVOID) {
    auto* r = new LoadResult();
    r->hr = LoadImpl(*r);
    PostMessageW(g_wnd, WM_LOADED, 0, (LPARAM)r);
    return 0;
}

// ---------------------------------------------------------------------------
// Applying changes
// ---------------------------------------------------------------------------
struct ProgCtx { size_t idx, count; };
static ProgCtx g_pctx;

static void CALLBACK OnProgress(UINT cur, UINT total, PVOID) {
    UINT frac = total ? (UINT)((unsigned long long)cur * 1000 / total) : 0;
    UINT overall = (UINT)((g_pctx.idx * 1000 + frac) / (g_pctx.count ? g_pctx.count : 1));
    PostMessageW(g_wnd, WM_PROGRESS, overall, 0);
}

static std::wstring PsQuote(const std::wstring& s) {
    std::wstring o = L"'";
    for (wchar_t c : s) { if (c == L'\'') o += L"''"; else o += c; }
    return o + L"'";
}

static void RunServerBatch(bool enable, const std::vector<std::wstring>& names, const ApplyArgs& a, ApplyResult& res) {
    std::wstring list;
    for (auto& n : names) { if (!list.empty()) list += L","; list += PsQuote(n); }
    std::wstring script = L"$ErrorActionPreference='Stop'; try { $r = ";
    if (enable) {
        script += L"Install-WindowsFeature -Name " + list + L" -IncludeManagementTools";
        if (!a.source.empty()) script += L" -Source " + PsQuote(a.source);
    } else {
        script += L"Uninstall-WindowsFeature -Name " + list;
        if (a.removePayload) script += L" -Remove";
    }
    script += L"; if ($r.Success) { 'OK|' + $r.RestartNeeded } else { 'ERR|ExitCode ' + $r.ExitCode } } "
              L"catch { 'ERR|' + ($_.Exception.Message -replace '[\\r\\n]+',' ') }";
    std::string raw;
    std::wstring out;
    if (RunCapture(PowerShellCmd(script), raw)) out = Utf8ToWide(raw);
    else out = L"ERR|PowerShell could not be started";
    size_t p = out.find(L"OK|");
    size_t e = out.rfind(L"ERR|");
    if (p != std::wstring::npos && (e == std::wstring::npos || p > e)) {
        res.ok += (int)names.size();
        if (out.find(L"OK|Yes", p) != std::wstring::npos || out.find(L"OK|Maybe", p) != std::wstring::npos) res.reboot = true;
    } else {
        res.failed += (int)names.size();
        std::wstring m = e != std::wstring::npos ? out.substr(e + 4) : out;
        while (!m.empty() && (m.back() == L'\r' || m.back() == L'\n')) m.pop_back();
        HRESULT eh = E_FAIL;
        size_t hp = Lower(m).find(L"0x8");
        bool hasCode = false;
        if (hp != std::wstring::npos) { eh = (HRESULT)wcstoul(m.c_str() + hp + 2, nullptr, 16); hasCode = true; }
        const wchar_t* plain = hasCode ? PlainError(eh) : nullptr;
        if ((hasCode && IsSourceProblem(eh)) || Lower(m).find(L"source files") != std::wstring::npos) res.sourceProblem = true;
        res.errors += std::wstring(enable ? L"Install" : L"Remove") + L" (server features): " + (plain ? plain : L"It didn't work.") +
                      L"\r\n    Details: " + m + L"\r\n";
        for (auto& n : names) res.fails.push_back({ n, enable });
    }
}

static DWORD WINAPI ApplyThread(LPVOID p) {
    std::unique_ptr<ApplyArgs> a((ApplyArgs*)p);
    auto* res = new ApplyResult();

    std::vector<std::wstring> srvOff, srvOn;
    std::vector<Job> dOff, dOn;
    for (auto& j : a->jobs) {
        if (j.kind == K_SERVER_FEATURE) (j.enable ? srvOn : srvOff).push_back(j.name);
        else (j.enable ? dOn : dOff).push_back(j);
    }
    std::stable_sort(dOff.begin(), dOff.end(), [](const Job& x, const Job& y) { return x.depth > y.depth; });
    std::stable_sort(dOn.begin(), dOn.end(), [](const Job& x, const Job& y) { return x.depth < y.depth; });

    size_t count = dOff.size() + dOn.size() + (srvOff.empty() ? 0 : 1) + (srvOn.empty() ? 0 : 1);
    g_pctx = { 0, count };
    auto step = [&](const std::wstring& what) {
        PostStatus(what + L"  [" + std::to_wstring(g_pctx.idx + 1) + L"/" + std::to_wstring(count) + L"]");
        PostMessageW(g_wnd, WM_PROGRESS, (WPARAM)(g_pctx.idx * 1000 / count), 0);
    };

    if (!srvOff.empty()) { step(L"Removing roles and features"); RunServerBatch(false, srvOff, *a, *res); ++g_pctx.idx; }

    auto doDism = [&](const Job& j) {
        if (j.kind == K_ROLL) {
            step(std::wstring(j.enable ? L"Baking " : L"Putting away ") + j.name.substr(5));
            RegPutU(j.name.c_str(), j.enable ? 1 : 0);
            ++res->ok; ++g_pctx.idx;
            return;
        }
        step(std::wstring(j.enable ? L"Turning on " : L"Turning off ") + j.name);
        HRESULT hr;
        const wchar_t* src = a->source.empty() ? nullptr : a->source.c_str();
        if (j.kind == K_DISM_FEATURE && j.enable)
            hr = g_api.EnableFeature(g_session, j.name.c_str(), nullptr, 0, FALSE, src ? &src : nullptr, src ? 1 : 0, TRUE, nullptr, OnProgress, nullptr);
        else if (j.kind == K_DISM_FEATURE)
            hr = g_api.DisableFeature(g_session, j.name.c_str(), nullptr, a->removePayload ? TRUE : FALSE, nullptr, OnProgress, nullptr);
        else if (j.enable)
            hr = g_api.AddCapability(g_session, j.name.c_str(), FALSE, src ? &src : nullptr, src ? 1 : 0, nullptr, OnProgress, nullptr);
        else
            hr = g_api.RemoveCapability(g_session, j.name.c_str(), nullptr, OnProgress, nullptr);
        if (hr == dism::kRebootRequired) { res->reboot = true; ++res->ok; }
        else if (SUCCEEDED(hr)) ++res->ok;
        else {
            ++res->failed;
            const wchar_t* plain = PlainError(hr);
            res->errors += j.name + L": " + (plain ? plain : L"It didn't work.") + L"\r\n    Details: " + DismError(hr) + L"\r\n";
            res->fails.push_back({ j.name, j.enable });
            if (j.enable && IsSourceProblem(hr)) res->sourceProblem = true;
        }
        ++g_pctx.idx;
    };
    for (auto& j : dOff) doDism(j);
    for (auto& j : dOn) doDism(j);

    if (!srvOn.empty()) { step(L"Installing roles and features"); RunServerBatch(true, srvOn, *a, *res); ++g_pctx.idx; }

    PostMessageW(g_wnd, WM_APPLIED, 0, (LPARAM)res);
    return 0;
}

// ---------------------------------------------------------------------------
// Replacing optionalfeatures.exe (Image File Execution Options redirect)
// ---------------------------------------------------------------------------
static const wchar_t* kIfeoKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\optionalfeatures.exe";

static bool IfeoIsSet() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeoKey, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    wchar_t v[1024] = {}; DWORD sz = sizeof(v) - sizeof(wchar_t), t = 0;
    bool ours = RegQueryValueExW(k, L"Debugger", nullptr, &t, (BYTE*)v, &sz) == ERROR_SUCCESS && wcsstr(v, L"WinFeatures") != nullptr;
    RegCloseKey(k);
    return ours;
}

static bool IfeoInstall(std::wstring& msg) {
    wchar_t self[MAX_PATH], pf[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\WinFeatures", pf, MAX_PATH);
    std::wstring dir = pf, target = dir + L"\\WinFeatures.exe";
    if (_wcsicmp(self, target.c_str()) != 0) {
        CreateDirectoryW(dir.c_str(), nullptr);
        if (!CopyFileW(self, target.c_str(), FALSE)) { msg = L"Could not copy the program to " + target; return false; }
    }
    std::wstring cmd = L"\"" + target + L"\" --ifeo";
    const REGSAM views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM view : views) {
        HKEY k;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kIfeoKey, 0, nullptr, 0, KEY_SET_VALUE | view, nullptr, &k, nullptr) != ERROR_SUCCESS) {
            msg = L"Could not write the registry key (are you elevated?)."; return false;
        }
        LSTATUS st = RegSetValueExW(k, L"Debugger", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
        if (st != ERROR_SUCCESS) { msg = L"Could not write the Debugger value."; return false; }
    }
    msg = L"Done. \"Turn Windows features on or off\" (optionalfeatures.exe) now opens WinFeatures.\r\n"
          L"Installed to: " + target + L"\r\nThe original Windows file was not modified.";
    return true;
}

static bool IfeoRemove(std::wstring& msg) {
    const REGSAM views[2] = { KEY_WOW64_64KEY, KEY_WOW64_32KEY };
    for (REGSAM view : views) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kIfeoKey, 0, KEY_SET_VALUE | KEY_QUERY_VALUE | view, &k) != ERROR_SUCCESS) continue;
        RegDeleteValueW(k, L"Debugger");
        DWORD values = 0, subkeys = 0;
        RegQueryInfoKeyW(k, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr, nullptr, nullptr, nullptr);
        RegCloseKey(k);
        if (values == 0 && subkeys == 0) RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kIfeoKey, view, 0);
    }
    msg = L"The original Windows optionalfeatures.exe is restored.\r\n(The copy in Program Files\\WinFeatures can be deleted manually.)";
    return true;
}

// ---------------------------------------------------------------------------
// Tree view
// ---------------------------------------------------------------------------
static std::vector<signed char> g_vis;

// ---------------------------------------------------------------------------
// Beginner help: plain-English knowledge base
// ---------------------------------------------------------------------------
// level: 0 = normal, 1 = advanced (most people never need it), 2 = risky when turned on
struct Kb { const wchar_t* pat; const wchar_t* plain; int level; };
static const Kb kKb[] = {
 { L"Microsoft-Windows-Subsystem-Linux", L"Runs Linux command-line tools and apps (such as Ubuntu) inside Windows. Choose this if you follow Linux tutorials or write software. Needs a restart.", 0 },
 { L"VirtualMachinePlatform", L"The core technology that lets Windows run lightweight virtual machines. Needed by WSL 2, Windows Sandbox and many Android emulators. Needs a restart.", 0 },
 { L"HypervisorPlatform", L"Lets other virtual machine programs (VirtualBox, QEMU, Android emulators) use Windows' built-in virtualization.", 1 },
 { L"Microsoft-Hyper-V*", L"Windows' own virtual machine software. Lets you run other operating systems in a window. Needs Windows Pro or Enterprise and CPU virtualization enabled in the BIOS. Needs a restart.", 0 },
 { L"Hyper-V*", L"Windows Server's virtual machine role. Lets this server host virtual machines.", 1 },
 { L"Containers-DisposableClientVM", L"Windows Sandbox: a temporary, isolated copy of Windows for trying suspicious files or apps safely. Everything is deleted when you close it.", 0 },
 { L"Containers", L"Support for running application containers (for example Docker). Mostly used by developers.", 1 },
 { L"NetFx3", L".NET Framework 3.5 - needed by some older programs and games. Windows downloads it from Windows Update when you turn it on.", 0 },
 { L"NET-Framework-Core", L".NET Framework 3.5 - needed by some older programs. Needs a source path if the server has no internet access.", 0 },
 { L"NetFx4*", L".NET Framework 4.x - used by many current programs. Leave it on.", 0 },
 { L"IIS-*", L"Part of IIS (Internet Information Services), Microsoft's web server. Only needed if you want to host websites on this PC.", 1 },
 { L"Web-*", L"Part of the Web Server (IIS) role, which lets this computer host websites and web apps.", 1 },
 { L"MSMQ*", L"Microsoft Message Queue - used by some older business applications to pass messages. Leave off unless an app asks for it.", 1 },
 { L"SMB1Protocol*", L"SMB 1.0 is a very old file-sharing protocol. It is slow and insecure (the WannaCry attack used it). Only turn it on if an old NAS, scanner or printer requires it.", 2 },
 { L"FS-SMB1", L"SMB 1.0 is a very old file-sharing protocol. It is slow and insecure (the WannaCry attack used it). Only turn it on if an old device requires it.", 2 },
 { L"SmbDirect", L"Speeds up file sharing on special high-end network cards. Not needed on normal PCs.", 1 },
 { L"TelnetClient", L"A very old, unencrypted way to connect to other computers by typing text. Mostly used by network technicians to test ports. Everything, including passwords, is sent in plain text.", 2 },
 { L"Telnet-Client", L"A very old, unencrypted way to connect to other computers by typing text. Everything, including passwords, is sent in plain text.", 2 },
 { L"TFTP", L"A basic, unencrypted file-transfer tool used with network equipment. Most people never need it.", 1 },
 { L"SimpleTCP", L"Tiny legacy network services (echo, daytime and so on). Not needed today.", 1 },
 { L"DirectPlay", L"Old multiplayer technology used by games from the 1990s and early 2000s. Turn it on only if an old game asks for it.", 0 },
 { L"LegacyComponents", L"Support for very old programs. Turn it on only if an old app asks for it.", 1 },
 { L"MediaPlayback", L"Windows media features that many apps rely on to play video and audio. Leave on.", 0 },
 { L"WindowsMediaPlayer", L"The classic Windows Media Player.", 0 },
 { L"Media.WindowsMediaPlayer*", L"The classic Windows Media Player app.", 0 },
 { L"Printing-*", L"Printing features. Most are on by default. 'Print to PDF' adds a virtual printer that saves documents as PDF files.", 0 },
 { L"Internet-Explorer-Optional*", L"The old Internet Explorer. Microsoft Edge can open old sites in 'IE mode' instead, so most people don't need this.", 1 },
 { L"Client-ProjFS", L"Windows Projected File System - used behind the scenes by developer tools such as VFS for Git.", 1 },
 { L"WorkFolders-Client", L"Syncs work files from a company server to this PC. Only useful if your employer uses Work Folders.", 1 },
 { L"SearchEngine-Client-Package", L"Windows Search, which powers fast file searching. Leave on.", 0 },
 { L"Windows-Defender*", L"Part of Microsoft Defender antivirus. Leave on.", 0 },
 { L"OpenSSH.Client*", L"Lets you connect securely to other computers and servers from the command line (the 'ssh' command). Handy for developers and admins.", 0 },
 { L"OpenSSH.Server*", L"Lets other computers connect to this PC over SSH. Only turn it on if you need remote command-line access; it opens a network door.", 1 },
 { L"Rsat.*", L"Remote Server Administration Tools: programs for managing Windows servers from this PC. Only useful for IT administrators.", 1 },
 { L"RSAT*", L"Remote Server Administration Tools: programs for managing servers. Only useful for IT administrators.", 1 },
 { L"Language.*", L"Language support for Windows: display language, handwriting, speech, text-to-speech or text recognition (OCR). Only add languages you use.", 0 },
 { L"App.StepsRecorder*", L"Records your clicks with screenshots so you can show a problem to someone who is helping you.", 0 },
 { L"Microsoft.Windows.PowerShell.ISE*", L"A window for writing and testing PowerShell scripts.", 1 },
 { L"AD-Domain-Services", L"Makes this server a domain controller (the sign-in and directory service for a company network). Adding the role is only the first step; the server must still be promoted.", 1 },
 { L"DNS", L"Lets this server translate names such as example.com into network addresses.", 1 },
 { L"DHCP", L"Hands out network addresses automatically to devices on your network.", 1 },
 { L"FS-FileServer", L"Lets this server share folders and files with other computers.", 0 },
 { L"Failover-Clustering", L"Lets several servers work together so services keep running if one fails.", 1 },
 { L"Windows-Server-Backup", L"Simple built-in backup and restore for this server.", 0 },
 { L"SNMP-Service", L"An older monitoring protocol. Only needed if a monitoring tool asks for it.", 1 },
 { L"Print-Services", L"Lets this server share printers on the network.", 0 },
};

static bool PatMatch(const std::wstring& name, const wchar_t* pat) {
    std::wstring n = Lower(name), p = Lower(pat);
    if (!p.empty() && p.back() == L'*') { p.pop_back(); return n.compare(0, p.size(), p) == 0; }
    return n == p;
}

static const Kb* FindKb(const Node& n) {
    for (const Kb& k : kKb) if (PatMatch(n.name, k.pat)) return &k;
    return nullptr;
}

static std::wstring PlainText(const Node& n) {
    std::wstring t;
    if (n.kind == K_ROLL) return L"Just for fun: rolls only save a tick mark in this app's own settings. They never change Windows.";
    if (const Kb* k = FindKb(n)) {
        t = k->plain;
        if (k->level == 1) t += L"\r\nAdvanced: most people never need this.";
        else if (k->level == 2) t += L"\r\nCaution: this is older technology that can weaken your PC's security. Leave it off unless something specific needs it.";
    } else if (n.type == L"Role") t = L"A role is a main job this server can do. Turning one on may add several supporting features.";
    else if (n.type == L"Role Service") t = L"An optional part of a role.";
    else if (n.type == L"Feature") t = L"A supporting capability of the server.";
    else t = L"There is no plain-English guide for this one yet. If you are unsure, leave it as it is; the Microsoft description above explains its purpose.";
    return t;
}

// ---------------------------------------------------------------------------
// Suggestions catalogue
// ---------------------------------------------------------------------------
// scope: 0 = any, 1 = Windows client only, 2 = Windows Server only. names: ';'-separated, trailing '*' = prefix.
struct Sug { const wchar_t* title; const wchar_t* why; const wchar_t* warn; bool turnOn; int scope; const wchar_t* names; };
static const Sug kSug[] = {
 { L"Run Linux on Windows (WSL)", L"Use Ubuntu, Debian and other Linux tools directly inside Windows. Great for programming and learning Linux.", L"Needs a restart.", true, 1, L"Microsoft-Windows-Subsystem-Linux;VirtualMachinePlatform" },
 { L"Create virtual machines (Hyper-V)", L"Run other operating systems in a window to test software safely. Needs Windows Pro or Enterprise and CPU virtualization enabled in the BIOS.", L"Needs a restart. Can conflict with some other virtual machine programs.", true, 1, L"Microsoft-Hyper-V-All" },
 { L"Open risky files safely (Windows Sandbox)", L"A disposable Windows desktop for testing suspicious downloads. Everything is deleted when you close it.", L"Needs Windows Pro or Enterprise, virtualization support and a restart.", true, 1, L"Containers-DisposableClientVM" },
 { L"Speed up Android emulators and VirtualBox", L"Lets other virtual machine programs use Windows' built-in virtualization instead of their own slower mode.", nullptr, true, 1, L"HypervisorPlatform" },
 { L"Run older programs that need .NET 3.5", L"Some older apps and games refuse to start without .NET Framework 3.5.", L"Windows downloads the files from Windows Update, so you need an internet connection (or a source path).", true, 0, L"NetFx3;NET-Framework-Core" },
 { L"Play old games (DirectPlay)", L"Old multiplayer games from the 1990s and early 2000s sometimes need this.", nullptr, true, 1, L"DirectPlay" },
 { L"Connect to other computers with SSH", L"Adds the 'ssh' command so you can securely reach servers and other computers from a terminal.", nullptr, true, 0, L"OpenSSH.Client*" },
 { L"Let other computers connect to this one with SSH", L"Adds an SSH server so you can control this computer from another one.", L"Opens a network door. Only turn on if you need it, and use strong passwords or keys.", true, 0, L"OpenSSH.Server*" },
 { L"Host websites on this PC (IIS)", L"Run a local web server for testing sites or hosting internal pages.", L"Only the basic pieces are selected; add more under Windows features if you need them.", true, 1, L"IIS-WebServerRole;IIS-WebServer;IIS-DefaultDocument;IIS-StaticContent;IIS-HttpErrors;IIS-ManagementConsole" },
 { L"Run containers (Docker)", L"Support for application containers, used by Docker and similar tools.", L"Needs a restart.", true, 1, L"Containers" },
 { L"Manage servers from this PC (admin tools)", L"Adds the Windows Server management consoles (Active Directory, Group Policy, DNS, DHCP, Server Manager).", L"Only useful for IT administrators.", true, 1, L"Rsat.ServerManager.Tools*;Rsat.ActiveDirectory.DS-LDS.Tools*;Rsat.GroupPolicy.Management.Tools*;Rsat.Dns.Tools*;Rsat.DHCP.Tools*" },
 { L"Turn off old, insecure file sharing (SMB 1.0)", L"SMB 1.0 is decades old and was used by the WannaCry attack. Modern devices do not need it.", L"Leave it on only if an old NAS, scanner or printer cannot connect without it.", false, 1, L"SMB1Protocol;SMB1Protocol-Client;SMB1Protocol-Server" },
 { L"Turn off Telnet client", L"Telnet sends everything, including passwords, unencrypted. SSH is the safe replacement.", nullptr, false, 1, L"TelnetClient" },
 { L"Turn off TFTP client", L"An old, unencrypted file-transfer tool that most people never use.", nullptr, false, 1, L"TFTP" },
 { L"Turn off legacy Simple TCP/IP services", L"Tiny legacy network services that nothing modern needs.", nullptr, false, 1, L"SimpleTCP" },
 { L"Turn off Internet Explorer", L"Edge can open old sites in IE mode, so the separate old browser is rarely needed.", nullptr, false, 1, L"Internet-Explorer-Optional*" },
 { L"Host websites and web apps (Web Server / IIS)", L"Lets this server host websites and web applications.", nullptr, true, 2, L"Web-Server;Web-Mgmt-Console" },
 { L"Run virtual machines (Hyper-V)", L"Turns this server into a host for virtual machines.", L"Needs a restart.", true, 2, L"Hyper-V;RSAT-Hyper-V-Tools;Hyper-V-PowerShell" },
 { L"Share files and folders (File Server)", L"Lets this server share folders with other computers.", nullptr, true, 2, L"FS-FileServer" },
 { L"Provide DNS name lookup", L"Lets this server answer name lookups for your network.", nullptr, true, 2, L"DNS" },
 { L"Hand out network addresses (DHCP)", L"Automatically gives devices on your network their addresses.", nullptr, true, 2, L"DHCP" },
 { L"Company sign-in directory (Active Directory)", L"Adds Active Directory Domain Services, the sign-in and directory service for company networks.", L"You still have to promote the server to a domain controller afterwards.", true, 2, L"AD-Domain-Services;RSAT-AD-AdminCenter" },
 { L"Back up this server", L"Simple built-in backup and restore.", nullptr, true, 2, L"Windows-Server-Backup" },
 { L"Keep services running if a server fails (clustering)", L"Lets several servers work together as a team.", nullptr, true, 2, L"Failover-Clustering" },
 { L"Turn off old, insecure file sharing (SMB 1.0)", L"SMB 1.0 is decades old and insecure. Modern devices do not need it.", L"Leave it on only if an old device cannot connect without it.", false, 2, L"FS-SMB1" },
 { L"Turn off Telnet client", L"Telnet sends everything, including passwords, unencrypted.", nullptr, false, 2, L"Telnet-Client" },
};

static std::vector<int> g_sugIdx;                 // indexes into kSug that apply to this PC
static std::vector<std::vector<int>> g_sugNodes;  // matching node indexes per applicable suggestion
static std::vector<HTREEITEM> g_sugH;

static void BuildSuggestions() {
    g_sugIdx.clear(); g_sugNodes.clear();
    bool serverMode = false;
    for (auto& n : g_nodes) if (n.kind == K_SERVER_FEATURE) serverMode = true;
    std::vector<char> claimed(g_nodes.size(), 0);
    for (size_t s = 0; s < sizeof(kSug) / sizeof(kSug[0]); ++s) {
        const Sug& g = kSug[s];
        if ((g.scope == 1 && serverMode) || (g.scope == 2 && !serverMode)) continue;
        std::vector<int> m;
        std::wstring names = g.names;
        size_t p = 0;
        while (p <= names.size()) {
            size_t e = names.find(L';', p);
            if (e == std::wstring::npos) e = names.size();
            std::wstring pat = names.substr(p, e - p);
            p = e + 1;
            if (pat.empty()) continue;
            for (size_t i = 0; i < g_nodes.size(); ++i)
                if (!claimed[i] && PatMatch(g_nodes[i].name, pat.c_str()) && std::find(m.begin(), m.end(), (int)i) == m.end())
                    m.push_back((int)i);
        }
        if (m.empty()) continue;
        if (!g.turnOn) {
            bool anyOn = false;
            for (int i : m) if (g_nodes[i].orig) anyOn = true;
            if (!anyOn) continue;
        }
        for (int i : m) claimed[i] = 1;
        g_sugIdx.push_back((int)s);
        g_sugNodes.push_back(m);
    }
}

// 1 = not applied, 2 = applied, 3 = partly applied
static int SugState(size_t k) {
    const Sug& g = kSug[g_sugIdx[k]];
    int done = 0;
    for (int i : g_sugNodes[k]) if (g_nodes[i].want == g.turnOn) ++done;
    return done == (int)g_sugNodes[k].size() ? 2 : done ? 3 : 1;
}
static bool SugPending(size_t k) {
    for (int i : g_sugNodes[k]) if (g_nodes[i].want != g_nodes[i].orig) return true;
    return false;
}
static bool SugAlreadyDone(size_t k) {
    const Sug& g = kSug[g_sugIdx[k]];
    for (int i : g_sugNodes[k]) if (g_nodes[i].orig != g.turnOn) return false;
    return true;
}
static void SugToggle(size_t k) {
    const Sug& g = kSug[g_sugIdx[k]];
    if (SugState(k) == 2) {
        for (int i : g_sugNodes[k]) g_nodes[i].want = g_nodes[i].orig;   // undo
        return;
    }
    for (int i : g_sugNodes[k]) {
        if (g.turnOn) {
            g_nodes[i].want = true;
            for (int p = g_nodes[i].parent; p >= 0; p = g_nodes[p].parent) g_nodes[p].want = true;
        } else SetSubtree(i, false);
    }
}

static std::wstring TipText(int i) {
    std::wstring t;
    if (i < 0) {
        size_t k = (size_t)(-i - 1);
        if (k < g_sugIdx.size()) t = kSug[g_sugIdx[k]].why;
    } else if (i < (int)g_nodes.size()) {
        const Node& n = g_nodes[i];
        t = PlainText(n);
        if (n.kind == K_ROLL) t = n.desc + L" (Just for fun: no system changes.)";
        else if (!FindKb(n) && n.type.empty() && !n.desc.empty()) t = n.desc;
    }
    if (t.size() > 420) t = t.substr(0, 420) + L"...";
    return t;
}

static void ShowGuide() {
    MessageBoxW(g_wnd,
        L"How to use this window\r\n\r\n"
        L"•  Tick a box to turn a feature on; untick it to turn it off. A filled square means only some parts are on.\r\n"
        L"•  Items you changed turn bold. Nothing happens until you click Apply.\r\n"
        L"•  Hover over any item for a plain-English explanation.\r\n"
        L"•  Not sure what to pick? Open the Suggestions tab for common goals (Linux, virtual machines, safer settings).\r\n"
        L"•  Some changes need a restart. You will be asked at the end.\r\n"
        L"•  Leave anything you don't recognise as it is.\r\n"
        L"•  Press Ctrl and + or - (or Ctrl and the mouse wheel) to make the text and boxes bigger or smaller.\r\n"
        L"•  Tick rolls in the Rolls tab to change the bread on the background.\r\n"
        L"•  Menu > Get an update from Microsoft Update Catalog downloads (and can install) a Windows update by KB number.\r\n"
        L"•  Menu > Fix missing files automatically lets the app find install media or repair Windows for you if a feature fails.",
        L"Quick guide", MB_OK | MB_ICONINFORMATION);
}

// ---------------------------------------------------------------------------
// Settings + theming (light / dark)
// ---------------------------------------------------------------------------
static DWORD RegGetU(const wchar_t* n, DWORD d) {
    DWORD v = d, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\WinFeatures", n, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
static void RegPutU(const wchar_t* n, DWORD v) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\WinFeatures", 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, n, 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static HRESULT (WINAPI* pSetWindowTheme)(HWND, LPCWSTR, LPCWSTR) = nullptr;

static void* (WINAPI* pOpenThemeData)(HWND, LPCWSTR) = nullptr;
static HRESULT (WINAPI* pDrawThemeBackground)(void*, HDC, int, int, const RECT*, const RECT*) = nullptr;
static HRESULT (WINAPI* pCloseThemeData)(void*) = nullptr;

static void InitUx() {
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    pSetWindowTheme = reinterpret_cast<decltype(pSetWindowTheme)>(GetProcAddress(ux, "SetWindowTheme"));
    pOpenThemeData = reinterpret_cast<decltype(pOpenThemeData)>(GetProcAddress(ux, "OpenThemeData"));
    pDrawThemeBackground = reinterpret_cast<decltype(pDrawThemeBackground)>(GetProcAddress(ux, "DrawThemeBackground"));
    pCloseThemeData = reinterpret_cast<decltype(pCloseThemeData)>(GetProcAddress(ux, "CloseThemeData"));
}

static LRESULT CALLBACK TabProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        { POINT o = { 0, 0 }; MapWindowPoints(h, GetParent(h), &o, 1); SetBrushOrgEx(dc, -o.x, -o.y, nullptr); }
        FillRect(dc, &rc, g_brBg);
        SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        int n = TabCtrl_GetItemCount(h), sel = TabCtrl_GetCurSel(h);
        HBRUSH acc = CreateSolidBrush(g_cAccent), line = CreateSolidBrush(g_cLine);
        RECT ln = { rc.left, rc.bottom - 1, rc.right, rc.bottom };
        FillRect(dc, &ln, line);
        for (int i = 0; i < n; ++i) {
            RECT r; TabCtrl_GetItemRect(h, i, &r);
            wchar_t t[64] = {};
            TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = t; ti.cchTextMax = 64;
            TabCtrl_GetItem(h, i, &ti);
            SetTextColor(dc, i == sel ? g_cText : g_cSub);
            DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (i == sel) { RECT u = { r.left + S(6), rc.bottom - S(3), r.right - S(6), rc.bottom }; FillRect(dc, &u, acc); }
        }
        DeleteObject(acc); DeleteObject(line);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, m, w, l);
}

static COLORREF Mix(COLORREF c, COLORREF bg, double a) {
    return RGB((int)(GetRValue(c) * a + GetRValue(bg) * (1 - a)),
               (int)(GetGValue(c) * a + GetGValue(bg) * (1 - a)),
               (int)(GetBValue(c) * a + GetBValue(bg) * (1 - a)));
}

// ---- Bread-roll background -------------------------------------------------
// Each tick in the Rolls tab changes which rolls (and toppings) are painted on the background.
struct RollStyle { COLORREF body, edge, hl; int shape, marks; };
// shape: 0 oval, 1 rounded square, 2 long thin, 3 flat wide
// marks: 0 none, 1 three slashes, 2 pinwheel, 3 one slash, 4 diagonal slashes, 5 fold, 6 flour, 7 salt, 8 top knot
static const RollStyle kStyles[10] = {
    { RGB(208,142,62),  RGB(146,88,30),  RGB(236,186,104), 0, 1 },   // 0 classic (nothing ticked)
    { RGB(226,170,90),  RGB(170,115,50), RGB(244,204,130), 0, 0 },   // 1 dinner
    { RGB(190,105,40),  RGB(120,60,20),  RGB(232,160,80),  0, 8 },   // 2 brioche
    { RGB(238,196,100), RGB(190,140,50), RGB(250,225,150), 1, 0 },   // 3 hawaiian
    { RGB(214,150,70),  RGB(150,92,32),  RGB(238,190,110), 0, 5 },   // 4 parker house
    { RGB(212,150,72),  RGB(150,95,35),  RGB(238,190,115), 0, 2 },   // 5 kaiser
    { RGB(226,190,140), RGB(170,130,80), RGB(244,225,190), 3, 6 },   // 6 ciabatta
    { RGB(214,155,80),  RGB(150,98,40),  RGB(238,195,125), 0, 3 },   // 7 papo seco
    { RGB(208,140,60),  RGB(146,88,30),  RGB(236,186,104), 2, 4 },   // 8 petit pain
    { RGB(140,80,40),   RGB(90,48,20),   RGB(180,120,70),  0, 7 },   // 9 pretzel
};
struct RollLook { std::vector<int> styles; bool butter = false, sesame = false, poppy = false, jam = false; };

static void DrawRollStyled(HDC dc, double s, double cx, double cy, double rx, double ry, COLORREF bg, double a,
                           int st, const RollLook& lk) {
    const RollStyle& rs = kStyles[st];
    auto P = [&](double v) { return (int)(v * s); };
    auto C = [&](COLORREF c) { return Mix(c, bg, a); };
    double w = rx, h = ry;
    if (rs.shape == 2) { w = rx * 1.3; h = ry * 0.55; }
    else if (rs.shape == 3) { w = rx * 1.15; h = ry * 0.72; }
    auto shape = [&](double x0, double y0, double x1, double y1) {
        if (rs.shape == 1) RoundRect(dc, P(x0), P(y0), P(x1), P(y1), P(34), P(34));
        else Ellipse(dc, P(x0), P(y0), P(x1), P(y1));
    };
    auto ell = [&](double x0, double y0, double x1, double y1) { Ellipse(dc, P(x0), P(y0), P(x1), P(y1)); };
    std::vector<HGDIOBJ> junk;
    auto brush = [&](COLORREF c) { HBRUSH b = CreateSolidBrush(C(c)); junk.push_back(b); SelectObject(dc, b); };
    auto pen = [&](double wd, COLORREF c) { HPEN p = CreatePen(PS_SOLID, std::max(1, P(wd)), C(c)); junk.push_back(p); SelectObject(dc, p); };
    HGDIOBJ oldPen = GetCurrentObject(dc, OBJ_PEN), oldBr = GetCurrentObject(dc, OBJ_BRUSH);
    HGDIOBJ nullPen = GetStockObject(NULL_PEN);
    auto line = [&](double x0, double y0, double x1, double y1) { MoveToEx(dc, P(x0), P(y0), nullptr); LineTo(dc, P(x1), P(y1)); };

    // shadow
    HBRUSH sh = CreateSolidBrush(Mix(RGB(120, 80, 40), bg, a * 0.45)); junk.push_back(sh);
    SelectObject(dc, sh); SelectObject(dc, nullPen);
    shape(cx - w + 5, cy - h + 11, cx + w + 5, cy + h + 11);
    // body
    brush(rs.body); pen(2.5, rs.edge);
    shape(cx - w, cy - h, cx + w, cy + h);
    // highlight
    brush(rs.hl); SelectObject(dc, nullPen);
    ell(cx - w * 0.66, cy - h * 0.74, cx + w * 0.22, cy + h * 0.02);
    if (lk.butter) { brush(RGB(255, 240, 170)); ell(cx - w * 0.45, cy - h * 0.62, cx + w * 0.05, cy - h * 0.18); }

    // marks (drawn twice: dark groove, then a light edge)
    for (int pass = 0; pass < 2; ++pass) {
        double off = pass ? 2.5 : 0;
        pen(pass ? 1.5 : 3.5, pass ? RGB(244, 205, 140) : rs.edge);
        switch (rs.marks) {
        case 1: for (int k = -1; k <= 1; ++k) { double x = cx + k * w * 0.36 + off; line(x - w * 0.10, cy - h * 0.42, x + w * 0.10, cy + h * 0.40); } break;
        case 2: for (int k = 0; k < 5; ++k) {
                    double a0 = k * 1.2566;
                    for (int i = 0; i < 8; ++i) {
                        double t0 = i / 8.0, t1 = (i + 1) / 8.0;
                        double x0 = cx + off + std::cos(a0 + t0 * 0.9) * t0 * w * 0.62, y0 = cy + std::sin(a0 + t0 * 0.9) * t0 * h * 0.62;
                        double x1 = cx + off + std::cos(a0 + t1 * 0.9) * t1 * w * 0.62, y1 = cy + std::sin(a0 + t1 * 0.9) * t1 * h * 0.62;
                        line(x0, y0, x1, y1);
                    } } break;
        case 3: line(cx - w * 0.6 + off, cy + h * 0.3, cx + w * 0.6 + off, cy - h * 0.3); break;
        case 4: for (int k = -1; k <= 1; ++k) { double x = cx + k * w * 0.42 + off; line(x - w * 0.09, cy - h * 0.5, x + w * 0.09, cy + h * 0.5); } break;
        case 5: line(cx - w * 0.8, cy + h * 0.12 + off, cx + w * 0.8, cy + h * 0.12 + off); break;
        default: break;
        }
    }
    SelectObject(dc, nullPen);
    if (rs.marks == 6) { brush(RGB(250, 245, 235)); for (int i = 0; i < 9; ++i) { double x = cx + w * (-0.7 + 0.18 * i), y = cy + h * (((i * 5) % 7) / 7.0 - 0.5); ell(x - 2.6, y - 2, x + 2.6, y + 2); } }
    if (rs.marks == 7) { brush(RGB(255, 255, 250)); for (int i = 0; i < 9; ++i) { double x = cx + w * (-0.6 + 0.15 * i), y = cy + h * (((i * 3) % 5) / 5.0 - 0.4); Rectangle(dc, P(x - 2.4), P(y - 2.4), P(x + 2.4), P(y + 2.4)); } }
    if (rs.marks == 8) { brush(rs.hl); pen(2, rs.edge); ell(cx - w * 0.2, cy - h * 0.95, cx + w * 0.2, cy - h * 0.45); SelectObject(dc, nullPen); }
    const double sx[4] = { -0.55, 0.52, 0.22, -0.22 }, sy[4] = { 0.38, -0.30, 0.66, -0.72 };
    if (lk.sesame) { brush(RGB(250, 236, 200)); for (int i = 0; i < 4; ++i) { double x = cx + w * sx[i], y = cy + h * sy[i]; ell(x - 3.2, y - 2.2, x + 3.2, y + 2.2); } }
    if (lk.poppy)  { brush(RGB(50, 40, 60)); for (int i = 0; i < 7; ++i) { double x = cx + w * (-0.6 + 0.2 * i), y = cy + h * (((i * 3) % 4) / 4.0 - 0.45); ell(x - 1.8, y - 1.8, x + 1.8, y + 1.8); } }
    if (lk.jam)    { brush(RGB(170, 30, 55)); ell(cx - w * 0.2, cy - h * 0.25, cx + w * 0.3, cy + h * 0.2); brush(RGB(220, 90, 110)); ell(cx - w * 0.05, cy - h * 0.15, cx + w * 0.1, cy); }

    SelectObject(dc, oldPen); SelectObject(dc, oldBr);
    for (HGDIOBJ o : junk) DeleteObject(o);
}

// Builds a seamless bread-roll tile as a pattern brush (rendered 3x and smoothed down).
static HBRUSH MakeTileBrush(int size, COLORREF bg, const RollLook& lk) {
    int big = size * 3;
    double s = big / 360.0, a = 0.08;   // a = strength of the rolls (lower = fainter)
    HDC scr = GetDC(nullptr);
    HDC bd = CreateCompatibleDC(scr);
    HBITMAP bb = CreateCompatibleBitmap(scr, big, big);
    HGDIOBJ ob = SelectObject(bd, bb);
    HBRUSH bgb = CreateSolidBrush(bg);
    RECT r = { 0, 0, big, big };
    FillRect(bd, &r, bgb);
    DeleteObject(bgb);
    static const double slots[5][4] = { { 85, 62, 50, 36 }, { 265, 62, 48, 35 }, { 175, 180, 54, 38 }, { 85, 298, 50, 36 }, { 265, 298, 50, 36 } };
    for (int i = 0; i < 5; ++i) {
        int st = lk.styles.empty() ? 0 : lk.styles[i % lk.styles.size()];
        DrawRollStyled(bd, s, slots[i][0], slots[i][1], slots[i][2], slots[i][3], bg, a, st, lk);
    }
    HDC sd = CreateCompatibleDC(scr);
    HBITMAP sb = CreateCompatibleBitmap(scr, size, size);
    HGDIOBJ os = SelectObject(sd, sb);
    SetStretchBltMode(sd, HALFTONE);
    SetBrushOrgEx(sd, 0, 0, nullptr);
    StretchBlt(sd, 0, 0, size, size, bd, 0, 0, big, big, SRCCOPY);
    SelectObject(sd, os);
    SelectObject(bd, ob);
    HBRUSH br = CreatePatternBrush(sb);
    DeleteObject(sb); DeleteObject(bb); DeleteDC(sd); DeleteDC(bd); ReleaseDC(nullptr, scr);
    return br;
}

// Reads the ticked rolls/toppings from the model (live, before Apply) and repaints the background.
static void CollectLook(RollLook& lk) {
    static const struct { const wchar_t* id; int style; } map[] = {
        { L"Roll-Dinner", 1 }, { L"Roll-Brioche", 2 }, { L"Roll-Hawaiian", 3 }, { L"Roll-ParkerHouse", 4 },
        { L"Roll-Kaiser", 5 }, { L"Roll-Ciabatta", 6 }, { L"Roll-PapoSeco", 7 }, { L"Roll-PetitPain", 8 }, { L"Roll-Pretzel", 9 },
    };
    for (const Node& n : g_nodes) {
        if (n.kind != K_ROLL || !n.want) continue;
        for (auto& m : map) if (n.name == m.id) lk.styles.push_back(m.style);
        if (n.name == L"Roll-Butter") lk.butter = true;
        if (n.name == L"Roll-Sesame") lk.sesame = true;
        if (n.name == L"Roll-Poppy")  lk.poppy = true;
        if (n.name == L"Roll-Jam")    lk.jam = true;
    }
    if (lk.styles.empty() && !lk.butter && !lk.sesame && !lk.poppy && !lk.jam) lk.sesame = true;   // classic default
}

static void RebuildTile() {
    RollLook lk;
    CollectLook(lk);
    HBRUSH old = g_brBg;
    g_brBg = MakeTileBrush(SD(300), g_cBg, lk);
    SetClassLongPtrW(g_wnd, GCLP_HBRBACKGROUND, (LONG_PTR)g_brBg);
    if (old) DeleteObject(old);
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void ThemeCtl(HWND h, const wchar_t* theme) {
    if (h && pSetWindowTheme) pSetWindowTheme(h, theme, nullptr);
}

// Draws the tree's check boxes ourselves so they scale with the text-size setting.
static void BuildCheckImages() {
    int sz = S(16);
    HIMAGELIST il = ImageList_Create(sz, sz, ILC_COLOR24, 4, 0);
    HDC scr = GetDC(nullptr);
    void* th = pOpenThemeData ? pOpenThemeData(g_tree, L"BUTTON") : nullptr;
    HBRUSH bg = CreateSolidBrush(g_cCtl);
    const int states[4] = { 0, 1, 5, 9 };   // blank, CBS_UNCHECKEDNORMAL, CBS_CHECKEDNORMAL, CBS_MIXEDNORMAL
    for (int k = 0; k < 4; ++k) {
        HDC dc = CreateCompatibleDC(scr);
        HBITMAP bmp = CreateCompatibleBitmap(scr, sz, sz);
        HGDIOBJ ob = SelectObject(dc, bmp);
        RECT r = { 0, 0, sz, sz };
        FillRect(dc, &r, bg);
        if (k > 0) {
            RECT b = { S(1), S(1), sz - S(1), sz - S(1) };
            if (th && pDrawThemeBackground) pDrawThemeBackground(th, dc, 3 /*BP_CHECKBOX*/, states[k], &b, nullptr);
            else DrawFrameControl(dc, &b, DFC_BUTTON, DFCS_BUTTONCHECK | (k == 2 ? DFCS_CHECKED : 0) | (k == 3 ? (DFCS_BUTTON3STATE | DFCS_CHECKED) : 0));
        }
        SelectObject(dc, ob);
        ImageList_Add(il, bmp, nullptr);
        DeleteObject(bmp); DeleteDC(dc);
    }
    DeleteObject(bg);
    if (th && pCloseThemeData) pCloseThemeData(th);
    ReleaseDC(nullptr, scr);
    HIMAGELIST old = TreeView_SetImageList(g_tree, il, TVSIL_STATE);
    if (old) ImageList_Destroy(old);
}

static void ApplyTheme() {
    g_cBg = GetSysColor(COLOR_BTNFACE); g_cCtl = GetSysColor(COLOR_WINDOW); g_cText = GetSysColor(COLOR_WINDOWTEXT);
    g_cSub = RGB(96, 96, 96); g_cAccent = RGB(0, 95, 184); g_cLine = RGB(200, 200, 200);
    HBRUSH oldCtl = g_brCtl;
    g_brCtl = CreateSolidBrush(g_cCtl);
    if (oldCtl) DeleteObject(oldCtl);
    RebuildTile();
    ThemeCtl(g_tree, L"Explorer");
    TreeView_SetBkColor(g_tree, g_cCtl);
    BuildCheckImages();
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void AddTip(HWND h, const wchar_t* text) {
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)h;
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(g_tt, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static const wchar_t* CatName(const Node& n) {
    switch (n.kind) {
    case K_ROLL: return L"Roll";
    case K_DISM_CAPABILITY: return L"Optional";
    case K_SERVER_FEATURE: return (n.type == L"Role" || n.type == L"Role Service") ? L"Role" : L"Feature";
    default: return L"Feature";
    }
}
static COLORREF CatColor(const Node& n) {
    switch (n.kind) {
    case K_ROLL: return RGB(150, 84, 10);
    case K_DISM_CAPABILITY: return RGB(0, 122, 76);
    case K_SERVER_FEATURE: return (n.type == L"Role" || n.type == L"Role Service") ? RGB(0, 84, 180) : (COLORREF)CLR_DEFAULT;
    default: return (COLORREF)CLR_DEFAULT;
    }
}

static std::wstring Label(const Node& n) {
    std::wstring s = n.display.empty() ? n.name : n.display;
    if (g_allTab >= 0 && g_cur == g_allTab) s = std::wstring(L"[") + CatName(n) + L"]  " + s;
    if (n.state == dism::InstallPending || n.state == dism::UninstallPending) s += L"  (restart pending)";
    else if (n.payloadRemoved) s += L"  (source needed)";
    return s;
}

static bool ComputeVis(int i, const std::wstring& f, int mode) {
    const Node& n = g_nodes[i];
    bool self = true;
    if (!f.empty()) self = Lower(n.display).find(f) != std::wstring::npos || Lower(n.name).find(f) != std::wstring::npos;
    if (self && mode == 1) self = n.orig;
    if (self && mode == 2) self = n.want != n.orig;
    bool any = self;
    for (int k : n.kids) if (ComputeVis(k, f, mode)) any = true;
    g_vis[i] = any;
    return any;
}

static void InsertNode(int i, HTREEITEM parent) {
    if (!g_vis[i]) return;
    Node& n = g_nodes[i];
    std::wstring t = Label(n);
    TVINSERTSTRUCTW is = {};
    is.hParent = parent; is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_PARAM;
    is.item.pszText = &t[0];
    is.item.lParam = i;
    n.h = TreeView_InsertItem(g_tree, &is);
    for (int k : n.kids) InsertNode(k, n.h);
}

static void RefreshChecks() {
    SendMessageW(g_tree, WM_SETREDRAW, FALSE, 0);
    for (size_t i = 0; i < g_nodes.size(); ++i) {
        Node& n = g_nodes[i];
        if (!n.h) continue;
        TVITEMW it = {};
        it.mask = TVIF_STATE; it.hItem = n.h;
        it.stateMask = TVIS_STATEIMAGEMASK | TVIS_BOLD;
        it.state = INDEXTOSTATEIMAGEMASK(Disp((int)i)) | (n.want != n.orig ? TVIS_BOLD : 0);
        TreeView_SetItem(g_tree, &it);
    }
    for (size_t k = 0; k < g_sugH.size(); ++k) {
        if (!g_sugH[k]) continue;
        TVITEMW it = {};
        it.mask = TVIF_STATE; it.hItem = g_sugH[k];
        it.stateMask = TVIS_STATEIMAGEMASK | TVIS_BOLD;
        it.state = INDEXTOSTATEIMAGEMASK(SugState(k)) | (SugPending(k) ? TVIS_BOLD : 0);
        TreeView_SetItem(g_tree, &it);
    }
    SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_tree, nullptr, TRUE);
}

static void ExpandAll(HTREEITEM h, UINT code) {
    for (; h; h = TreeView_GetNextSibling(g_tree, h)) {
        TreeView_Expand(g_tree, h, code);
        ExpandAll(TreeView_GetChild(g_tree, h), code);
    }
}

static void UpdateControls() {
    HWND ws[] = { g_tab, g_search, g_filter, g_expand, g_collapse, g_tree, g_src, g_browse, g_payload, g_refresh };
    for (HWND w : ws) EnableWindow(w, !g_busy);
    int n = PendingCount();
    EnableWindow(g_apply, !g_busy && n > 0);
    SetWindowTextW(g_apply, (L"Apply (" + std::to_wstring(n) + L")").c_str());
}

static void PopulateSuggestions(const std::wstring& f) {
    int shown = 0;
    for (size_t k = 0; k < g_sugIdx.size(); ++k) {
        const Sug& g = kSug[g_sugIdx[k]];
        if (!f.empty() && Lower(g.title).find(f) == std::wstring::npos && Lower(g.why).find(f) == std::wstring::npos) continue;
        std::wstring t = g.title;
        if (SugAlreadyDone(k)) t += L"   (already done)";
        TVINSERTSTRUCTW is = {};
        is.hParent = TVI_ROOT; is.hInsertAfter = TVI_LAST;
        is.item.mask = TVIF_TEXT | TVIF_PARAM;
        is.item.pszText = &t[0];
        is.item.lParam = -(LPARAM)(k + 1);
        HTREEITEM hh = TreeView_InsertItem(g_tree, &is);
        g_sugH[k] = hh;
        ++shown;
        for (int i : g_sugNodes[k]) {
            Node& n = g_nodes[i];
            std::wstring lt = Label(n);
            TVINSERTSTRUCTW cs = {};
            cs.hParent = hh; cs.hInsertAfter = TVI_LAST;
            cs.item.mask = TVIF_TEXT | TVIF_PARAM;
            cs.item.pszText = &lt[0];
            cs.item.lParam = i;
            n.h = TreeView_InsertItem(g_tree, &cs);
        }
    }
    ExpandAll(TreeView_GetRoot(g_tree), TVE_EXPAND);
    SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);
    RefreshChecks();
    SetWindowTextW(g_dtitle, L"");
    SetWindowTextW(g_desc, L"Common goals and safer settings for this PC. Tick a suggestion to select the features under it, then click Apply.");
    SetStatus(std::to_wstring(shown) + L" suggestions");
    UpdateControls();
}

static void Populate() {
    wchar_t buf[256];
    GetWindowTextW(g_search, buf, 256);
    std::wstring f = Lower(buf);
    int mode = (int)SendMessageW(g_filter, CB_GETCURSEL, 0, 0);
    if (mode < 0) mode = 0;

    SendMessageW(g_tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_tree);
    for (auto& n : g_nodes) n.h = nullptr;
    g_sugH.assign(g_sugIdx.size(), nullptr);
    if (g_sugTab >= 0 && g_cur == g_sugTab) { PopulateSuggestions(f); return; }
    g_vis.assign(g_nodes.size(), 0);
    int total = 0, shown = 0;
    for (size_t i = 0; i < g_nodes.size(); ++i)
        if ((g_cur == g_allTab || g_nodes[i].tab == g_cur) && g_nodes[i].parent == -1) ComputeVis((int)i, f, mode);
    for (size_t i = 0; i < g_nodes.size(); ++i) {
        if (g_cur != g_allTab && g_nodes[i].tab != g_cur) continue;
        ++total;
        if (g_vis[i]) ++shown;
        if (g_nodes[i].parent == -1) InsertNode((int)i, TVI_ROOT);
    }
    if (!f.empty() || mode != 0) ExpandAll(TreeView_GetRoot(g_tree), TVE_EXPAND);
    SendMessageW(g_tree, WM_SETREDRAW, TRUE, 0);
    RefreshChecks();
    SetWindowTextW(g_dtitle, g_cur == g_allTab ? L"Everything in one list" : L"");
    SetWindowTextW(g_desc, g_cur == g_allTab
        ? L"Every item from every tab. The label in front of each name says what it is:\r\n"
          L"[Feature] Windows or server feature (black)     [Role] server role or role service (blue)\r\n"
          L"[Optional] optional feature / Feature on Demand (green)     [Roll] bread roll, just for fun (brown)"
        : L"");
    SetStatus(std::to_wstring(shown) + L" of " + std::to_wstring(total) + L" shown");
    UpdateControls();
}

static void ShowDescription(int i) {
    if (i < 0) {
        size_t k = (size_t)(-i - 1);
        if (k >= g_sugIdx.size()) return;
        const Sug& g = kSug[g_sugIdx[k]];
        SetWindowTextW(g_dtitle, g.title);
        std::wstring t = g.why;
        if (g.warn) t += std::wstring(L"\r\n\r\nGood to know: ") + g.warn;
        t += L"\r\n\r\nTick the box to select every feature listed underneath. Nothing changes until you click Apply.";
        SetWindowTextW(g_desc, t.c_str());
        return;
    }
    if (i >= (int)g_nodes.size()) return;
    const Node& n = g_nodes[i];
    SetWindowTextW(g_dtitle, (n.display.empty() ? n.name : n.display).c_str());
    std::wstring t = n.desc;
    if (g_beginner) {
        std::wstring p = PlainText(n);
        if (!p.empty()) { if (!t.empty()) t += L"\r\n\r\n"; t += L"In plain English: " + p; }
    }
    if (!t.empty()) t += L"\r\n\r\n";
    t += L"Name: " + n.name;
    if (!n.type.empty()) t += L"\r\nType: " + n.type;
    if (n.kind == K_DISM_CAPABILITY && (n.dlSize || n.instSize))
        t += L"\r\nDownload: " + std::to_wstring(n.dlSize / 1024) + L" KB    Install: " + std::to_wstring(n.instSize / 1024) + L" KB";
    if (n.restart == 2) t += L"\r\nRestart required.";
    else if (n.restart == 1) t += L"\r\nRestart may be required.";
    if (n.payloadRemoved) t += L"\r\nThe payload was removed; a source path is needed to turn this on.";
    SetWindowTextW(g_desc, t.c_str());
}

static void Toggle(HTREEITEM h) {
    TVITEMW it = {}; it.mask = TVIF_PARAM; it.hItem = h;
    if (!TreeView_GetItem(g_tree, &it)) return;
    int i = (int)it.lParam;
    if (i < 0) {
        size_t k = (size_t)(-i - 1);
        if (k < g_sugIdx.size()) SugToggle(k);
        RefreshChecks();
        UpdateControls();
        return;
    }
    if (i >= (int)g_nodes.size()) return;
    int d = Disp(i);
    if (d == 2) {
        SetSubtree(i, false);
    } else if (d == 3 && g_nodes[i].want) {
        SetSubtree(i, true);
    } else {
        g_nodes[i].want = true;
        for (int p = g_nodes[i].parent; p >= 0; p = g_nodes[p].parent) g_nodes[p].want = true;
    }
    RefreshChecks();
    UpdateControls();
    if (g_nodes[i].kind == K_ROLL) RebuildTile();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void StartLoad() {
    g_busy = true;
    UpdateControls();
    SetStatus(L"Loading...");
    SetWindowLongPtrW(g_prog, GWL_STYLE, GetWindowLongPtrW(g_prog, GWL_STYLE) | PBS_MARQUEE);
    SendMessageW(g_prog, PBM_SETMARQUEE, TRUE, 30);
    HANDLE h = CreateThread(nullptr, 0, LoadThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

static void StopMarquee() {
    SendMessageW(g_prog, PBM_SETMARQUEE, FALSE, 0);
    SetWindowLongPtrW(g_prog, GWL_STYLE, GetWindowLongPtrW(g_prog, GWL_STYLE) & ~PBS_MARQUEE);
    SendMessageW(g_prog, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
}

static void DoApply(bool skipConfirm = false) {
    auto* a = new ApplyArgs();
    for (auto& n : g_nodes)
        if (n.want != n.orig) a->jobs.push_back({ n.kind, n.want, n.depth, n.name });
    if (a->jobs.empty()) { delete a; return; }
    wchar_t buf[MAX_PATH] = {};
    GetWindowTextW(g_src, buf, MAX_PATH);
    a->source = buf;
    a->removePayload = SendMessageW(g_payload, BM_GETCHECK, 0, 0) == BST_CHECKED;

    std::wstring msg;
    if (g_beginner) {
        std::wstring on, off, warn;
        int nOn = 0, nOff = 0;
        auto add = [](std::wstring& list, int& cnt, const std::wstring& d) {
            if (cnt < 10) list += L"\r\n  \u2022 " + d;
            ++cnt;
        };
        for (auto& n : g_nodes) {
            if (n.want == n.orig) continue;
            std::wstring d = n.display.empty() ? n.name : n.display;
            if (n.want) add(on, nOn, d); else add(off, nOff, d);
            const Kb* k = FindKb(n);
            if (k && k->level == 2 && n.want) warn += L"\r\n  \u2022 " + d;
        }
        msg = L"You are about to change this PC.";
        if (nOn) { msg += L"\r\n\r\nTurn ON (" + std::to_wstring(nOn) + L"):" + on; if (nOn > 10) msg += L"\r\n  ...and " + std::to_wstring(nOn - 10) + L" more"; }
        if (nOff) { msg += L"\r\n\r\nTurn OFF (" + std::to_wstring(nOff) + L"):" + off; if (nOff > 10) msg += L"\r\n  ...and " + std::to_wstring(nOff - 10) + L" more"; }
        if (!warn.empty()) msg += L"\r\n\r\nHeads-up: these are older features that can weaken security:" + warn;
        msg += L"\r\n\r\nThis can take several minutes and some changes need a restart. Continue?";
    } else {
        msg = L"Apply " + std::to_wstring(a->jobs.size()) + L" change(s)? This can take several minutes.";
    }
    if (!skipConfirm && MessageBoxW(g_wnd, msg.c_str(), L"Windows Features", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) { delete a; return; }

    g_busy = true;
    UpdateControls();
    SendMessageW(g_prog, PBM_SETRANGE32, 0, 1000);
    SendMessageW(g_prog, PBM_SETPOS, 0, 0);
    HANDLE h = CreateThread(nullptr, 0, ApplyThread, a, 0, nullptr);
    if (h) CloseHandle(h);
}

static void RebootNow() {
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &tp.Privileges[0].Luid);
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(tok);
    }
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_RECONFIG | SHTDN_REASON_FLAG_PLANNED);
}

// ---- Finding install media (source files) --------------------------------------
// Looks for Windows install media (sources\sxs) or Features on Demand media on every local/removable/optical drive.
static std::vector<std::wstring> FindMedia(bool preferFod) {
    std::vector<std::wstring> sxs, fod;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[4] = { (wchar_t)(L'A' + i), L':', L'\\', 0 };
        UINT t = GetDriveTypeW(root);
        if (t == DRIVE_NO_ROOT_DIR || t == DRIVE_UNKNOWN || t == DRIVE_REMOTE) continue;
        auto isDir = [](const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); };
        std::wstring r = root;
        if (isDir(r + L"sources\\sxs")) sxs.push_back(r + L"sources\\sxs");
        else if (isDir(r + L"sxs")) sxs.push_back(r + L"sxs");
        if (isDir(r + L"LanguagesAndOptionalFeatures")) fod.push_back(r + L"LanguagesAndOptionalFeatures");
    }
    std::vector<std::wstring> out = preferFod ? fod : sxs;
    out.insert(out.end(), preferFod ? sxs.begin() : fod.begin(), preferFod ? sxs.end() : fod.end());
    return out;
}

static std::wstring MediaLabel(const std::wstring& p) {
    return p + (p.find(L"LanguagesAndOptionalFeatures") != std::wstring::npos ? L"   (Features on Demand media)" : L"   (Windows install media)");
}

static void UseSource(const std::wstring& p) {
    SetWindowTextW(g_src, p.c_str());
    SetStatus(L"Source path set to " + p);
}

static void ChooseFromMedia(const std::vector<std::wstring>& media) {
    if (media.empty()) {
        MessageBoxW(g_wnd, L"No install media was found.\\r\\n\\r\\nMount a Windows ISO that matches your version of Windows (or insert the disc), "
                           L"then try again. You can also use 'Choose an ISO file...' and I will mount it for you.",
                    L"Windows Features", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (media.size() == 1) { UseSource(media[0]); return; }
    HMENU m = CreatePopupMenu();
    for (size_t i = 0; i < media.size() && i < 20; ++i) AppendMenuW(m, MF_STRING, 320 + (UINT)i, MediaLabel(media[i]).c_str());
    RECT rc; GetWindowRect(g_browse, &rc);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, rc.left, rc.bottom, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (cmd >= 320 && cmd < 340) UseSource(media[cmd - 320]);
}

static void PickIso() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Disc images (*.iso)\0*.iso\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Choose a Windows ISO that matches your version of Windows";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    SetStatus(L"Mounting the ISO...");
    std::string raw;
    bool ran = RunCapture(PowerShellCmd(L"try { Mount-DiskImage -ImagePath " + PsQuote(file) +
                          L" -ErrorAction Stop | Out-Null; 'OK' } catch { 'ERR|' + ($_.Exception.Message -replace '[\\r\\n]+',' ') }"), raw);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    std::wstring out = ran ? Utf8ToWide(raw) : L"ERR|PowerShell could not be started";
    if (out.find(L"OK") == std::wstring::npos) {
        MessageBoxW(g_wnd, (L"The ISO could not be mounted.\\r\\n\\r\\n" + out).c_str(), L"Windows Features", MB_OK | MB_ICONWARNING);
        return;
    }
    auto media = FindMedia(false);
    if (media.empty()) MessageBoxW(g_wnd, L"The ISO is mounted, but it doesn't contain install files (sources\\sxs). Is it the right disc?",
                                   L"Windows Features", MB_OK | MB_ICONWARNING);
    else ChooseFromMedia(media);
}

static void PickFolder() {
    BROWSEINFOW bi = {};
    bi.hwndOwner = g_wnd;
    bi.lpszTitle = L"Select the source folder (for example D:\\sources\\sxs):";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, path)) UseSource(path);
        CoTaskMemFree(pidl);
    }
}

static void ShowFindMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 301, L"Search my drives for install media");
    AppendMenuW(m, MF_STRING, 302, L"Choose an ISO file (I'll mount it)...");
    AppendMenuW(m, MF_STRING, 303, L"Choose a folder...");
    RECT rc; GetWindowRect(g_browse, &rc);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN, rc.left, rc.bottom, 0, g_wnd, nullptr);
    DestroyMenu(m);
    if (cmd == 301) { SetCursor(LoadCursorW(nullptr, IDC_WAIT)); auto media = FindMedia(false); SetCursor(LoadCursorW(nullptr, IDC_ARROW)); ChooseFromMedia(media); }
    else if (cmd == 302) PickIso();
    else if (cmd == 303) PickFolder();
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
static BOOL CALLBACK SetFontProc(HWND h, LPARAM) { SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE); return TRUE; }

static void MakeFonts() {
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    LOGFONTW lf = {};
    lf.lfHeight = -MulDiv(9, g_dpi * g_textPct / 100, 72);
    lf.lfWeight = FW_NORMAL;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy(lf.lfFaceName, L"Segoe UI");
    g_font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = -MulDiv(10, g_dpi * g_textPct / 100, 72);
    g_fontBold = CreateFontIndirectW(&lf);
    EnumChildWindows(g_wnd, SetFontProc, 0);
    SendMessageW(g_dtitle, WM_SETFONT, (WPARAM)g_fontBold, TRUE);
}

static void Layout() {
    RECT rc; GetClientRect(g_wnd, &rc);
    int w = rc.right, h = rc.bottom;
    const int m = S(12);
    int y = m;
    int tabH = S(34), rowH = S(26), btnH = S(30);
    int menuW = S(84);

    MoveWindow(g_tab, m, y, w - 2 * m - menuW - S(6), tabH, TRUE);
    MoveWindow(g_menuBtn, w - m - menuW, y + S(2), menuW, tabH - S(6), TRUE);
    y += tabH + S(8);

    int bx = w - m;
    bx -= S(84); MoveWindow(g_collapse, bx, y, S(84), rowH, TRUE); bx -= S(6);
    bx -= S(84); MoveWindow(g_expand, bx, y, S(84), rowH, TRUE);   bx -= S(6);
    bx -= S(130); MoveWindow(g_filter, bx, y, S(130), S(200), TRUE); bx -= S(6);
    MoveWindow(g_search, m, y, bx - m, rowH, TRUE);            y += rowH + S(8);

    int bottom = h - m;
    int rApply = bottom - btnH;
    MoveWindow(g_apply, w - m - S(110), rApply, S(110), btnH, TRUE);
    MoveWindow(g_refresh, w - m - S(110) - S(6) - S(84), rApply, S(84), btnH, TRUE);
    int leftW = w - 2 * m - S(110) - S(84) - S(20);
    MoveWindow(g_prog, m, rApply + S(2), leftW, S(10), TRUE);
    MoveWindow(g_status, m, rApply + S(14), leftW, S(18), TRUE);

    int ySrc = rApply - S(8) - rowH;
    MoveWindow(g_srcLbl, m, ySrc + S(4), S(90), S(20), TRUE);
    MoveWindow(g_src, m + S(90), ySrc, w - 2 * m - S(90) - S(78), rowH, TRUE);
    MoveWindow(g_browse, w - m - S(72), ySrc, S(72), rowH, TRUE);

    int yTop = ySrc;
    if (g_isServer) {
        yTop = ySrc - S(4) - S(22);
        MoveWindow(g_payload, m, yTop, w - 2 * m, S(22), TRUE);
    }
    int descH = S(72), titleH = S(24);
    int yDesc = yTop - S(8) - descH;
    MoveWindow(g_desc, m, yDesc, w - 2 * m, descH, TRUE);
    int yTitle = yDesc - titleH;
    MoveWindow(g_dtitle, m, yTitle, w - 2 * m, titleH, TRUE);
    MoveWindow(g_tree, m, y, w - 2 * m, yTitle - S(6) - y, TRUE);
    TreeView_SetItemHeight(g_tree, S(24));
}

static void ApplyTextSize() {
    MakeFonts();
    BuildCheckImages();
    SendMessageW(g_tt, TTM_SETMAXTIPWIDTH, 0, S(360));
    RECT wr; GetWindowRect(g_wnd, &wr);
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = std::min<int>(std::max<int>(wr.right - wr.left, S(700)), wa.right - wa.left);
    int h = std::min<int>(std::max<int>(wr.bottom - wr.top, S(640)), wa.bottom - wa.top);
    SetWindowPos(g_wnd, nullptr, wr.left, wr.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    RedrawWindow(g_wnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD ex, int id) {
    return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// Microsoft Update Catalog downloader + automatic repair
// ---------------------------------------------------------------------------
static HWND g_cat, g_catQ, g_catGo, g_catList, g_catAuto, g_catGet, g_catProg, g_catStat;
static std::vector<cat::Result> g_catRes;
static bool g_catBusy = false;       // a search/download/install is running
static bool g_catHoldsMain = false;  // the catalog job has set g_busy
static bool g_autoFix = false;       // fix missing source files automatically
static bool g_repairTried = false;   // guard against repair loops

static void CatStatus(const std::wstring& t) {
    if (g_cat) PostMessageW(g_cat, WM_CAT_STATUS, 0, (LPARAM) new std::wstring(t));
    PostStatus(t);
}
static void CatProgress(UINT permille) {
    if (g_cat) PostMessageW(g_cat, WM_CAT_PROG, permille, 0);
    PostMessageW(g_wnd, WM_PROGRESS, permille, 0);
}
static void CatDone(int kind, const std::wstring& t) {
    PostMessageW(g_cat, WM_CAT_DONE, kind, (LPARAM) new std::wstring(t));
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// GET (post == nullptr) or POST a form. Body goes to outStr, or to outFile if given.
static bool HttpFetch(const std::string& url, const std::string* post, std::string* outStr, HANDLE outFile,
                      const std::function<void(ULONGLONG, ULONGLONG)>& prog, DWORD& status) {
    status = 0;
    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc = {}; uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    std::wstring full = std::wstring(path, uc.dwUrlPathLength) + std::wstring(uc.lpszExtraInfo ? uc.lpszExtraInfo : L"", uc.dwExtraInfoLength == (DWORD)-1 ? 0 : uc.dwExtraInfoLength);
    HINTERNET s = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) WinFeatures/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    bool ok = false;
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0), r = nullptr;
    if (c) {
        DWORD fl = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        r = WinHttpOpenRequest(c, post ? L"POST" : L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, fl);
    }
    if (r) {
        const wchar_t* hdr = post ? L"Content-Type: application/x-www-form-urlencoded" : WINHTTP_NO_ADDITIONAL_HEADERS;
        DWORD hl = post ? (DWORD)-1 : 0;
        if (WinHttpSendRequest(r, hdr, hl, post ? (LPVOID)post->data() : WINHTTP_NO_REQUEST_DATA, post ? (DWORD)post->size() : 0,
                               post ? (DWORD)post->size() : 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            ULONGLONG total = 0, got = 0;
            wchar_t lenbuf[32]; sz = sizeof(lenbuf);
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, lenbuf, &sz, nullptr)) total = _wcstoui64(lenbuf, nullptr, 10);
            ok = status == 200;
            std::vector<char> buf(64 * 1024);
            DWORD n;
            while (ok && WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n) && n) {
                if (outFile) { DWORD w; if (!WriteFile(outFile, buf.data(), n, &w, nullptr) || w != n) { ok = false; break; } }
                else if (outStr) outStr->append(buf.data(), n);
                got += n;
                if (prog) prog(got, total);
            }
            if (ok && GetLastError() == ERROR_WINHTTP_TIMEOUT) ok = false;
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

// True only if the file has a valid signature and the signer is Microsoft.
static bool VerifyMicrosoft(const std::wstring& file, std::wstring& signer) {
    static const GUID kGeneric = { 0x00AAC56B, 0xCD44, 0x11d0, { 0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE } };
    GUID g = kGeneric;
    WINTRUST_FILE_INFO fi = {}; fi.cbStruct = sizeof(fi); fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA wd = {}; wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE; wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE; wd.pFile = &fi; wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG rc = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &g, &wd);
    if (rc == 0) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (pc && pc->pCert) {
            wchar_t nm[256] = {};
            CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nm, 256);
            signer = nm;
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &g, &wd);
    return rc == 0 && (signer == L"Microsoft Corporation" || signer == L"Microsoft Windows");
}

static std::wstring MB(ULONGLONG b) { return std::to_wstring(b / (1024 * 1024)) + L"." + std::to_wstring((b / (1024 * 102)) % 10) + L" MB"; }

static void CALLBACK CatDismCb(UINT cur, UINT total, PVOID) { CatProgress(total ? (UINT)((unsigned long long)cur * 1000 / total) : 0); }

struct SearchArgs { std::string q; };
static DWORD WINAPI CatSearchThread(LPVOID p) {
    std::unique_ptr<SearchArgs> a((SearchArgs*)p);
    CatStatus(L"Searching the Microsoft Update Catalog...");
    CatProgress(0);
    std::string html; DWORD st = 0;
    bool ok = HttpFetch("https://www.catalog.update.microsoft.com/Search.aspx?q=" + cat::UrlEncode(a->q), nullptr, &html, nullptr, nullptr, st);
    if (!ok) {
        CatDone(3, st ? L"The catalog answered with an error (code " + std::to_wstring(st) + L"). Try again in a moment."
                      : L"Couldn't reach the Microsoft Update Catalog. Check your internet connection (or proxy/firewall) and try again.");
        return 0;
    }
    SYSTEM_INFO si; GetNativeSystemInfo(&si);
    bool arm = si.wProcessorArchitecture == 12;
    auto all = cat::ParseSearch(html);
    auto* res = new std::vector<cat::Result>();
    for (auto& r : all) if (cat::MatchesArch(r.title, arm)) res->push_back(r);
    size_t n = res->size();
    PostMessageW(g_cat, WM_CAT_RESULTS, 0, (LPARAM)res);
    CatDone(0, n ? std::to_wstring(n) + L" result(s) for this PC. Pick one and click Get."
                 : (all.empty() ? L"Nothing found. Try a KB number such as KB5034441."
                                : L"Results were found, but none for this PC's processor type."));
    return 0;
}

struct GetArgs { std::string guid, title; bool autoInstall; };
static DWORD WINAPI CatGetThread(LPVOID p) {
    std::unique_ptr<GetArgs> a((GetArgs*)p);
    CatProgress(0);
    CatStatus(L"Asking the catalog for the download link...");
    std::string body = cat::BuildDownloadForm(a->guid), js; DWORD st = 0;
    if (!HttpFetch("https://www.catalog.update.microsoft.com/DownloadDialog.aspx", &body, &js, nullptr, nullptr, st)) {
        CatDone(3, L"Couldn't get the download link from the catalog. Check your connection and try again.");
        return 0;
    }
    std::string url;
    for (auto& u : cat::ParseDownloadUrls(js)) if (cat::HostAllowed(u) && cat::IsPackageFile(cat::FileNameFromUrl(u))) { url = u; break; }
    if (url.empty()) for (auto& u : cat::ParseDownloadUrls(js)) if (cat::HostAllowed(u)) { url = u; break; }
    if (url.empty()) { CatDone(3, L"The catalog didn't give a download link I trust (it must be on a Microsoft update server)."); return 0; }

    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"WinFeatures";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring name = Utf8ToWide(cat::FileNameFromUrl(url));
    std::wstring file = dir + L"\\" + name;
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { CatDone(3, L"Couldn't create the download file in the temporary folder."); return 0; }
    DWORD lastTick = 0;
    bool ok = HttpFetch(url, nullptr, nullptr, h, [&](ULONGLONG got, ULONGLONG total) {
        DWORD t = GetTickCount();
        if (t - lastTick < 100) return;
        lastTick = t;
        CatProgress(total ? (UINT)(got * 1000 / total) : 0);
        CatStatus(L"Downloading " + name + L": " + MB(got) + (total ? L" of " + MB(total) : L""));
    }, st);
    CloseHandle(h);
    if (!ok) { DeleteFileW(file.c_str()); CatDone(3, L"The download stopped before it finished. Check your connection and try again."); return 0; }
    CatProgress(1000);
    CatStatus(L"Checking Microsoft's signature...");
    std::wstring signer;
    bool signedOk = VerifyMicrosoft(file, signer);
    if (!signedOk) {
        if (a->autoInstall) {
            DeleteFileW(file.c_str());
            CatDone(3, L"I downloaded the file but couldn't confirm it is signed by Microsoft, so I deleted it and did NOT install it.");
        } else {
            CatDone(1, L"Saved to " + file + L"\r\n(Warning: I couldn't confirm the Microsoft signature.)");
        }
        return 0;
    }
    if (!a->autoInstall) { CatDone(1, L"Saved to " + file + L"\r\nSigned by " + signer + L". Tick 'Install automatically' next time to install it for you."); return 0; }

    CatStatus(L"Installing (this can take several minutes)...");
    CatProgress(0);
    HRESULT hr = g_api.AddPackage ? g_api.AddPackage(g_session, file.c_str(), FALSE, FALSE, nullptr, CatDismCb, nullptr) : E_NOTIMPL;
    if (SUCCEEDED(hr) || hr == dism::kRebootRequired) {
        DeleteFileW(file.c_str());
        CatProgress(1000);
        CatDone(hr == dism::kRebootRequired ? 4 : 2, L"Installed. Signed by " + signer + L".");
    } else {
        const wchar_t* plain = PlainError(hr);
        std::wstring m = L"Windows couldn't install it. ";
        m += plain ? plain : DismError(hr).c_str();
        CatDone(3, m + L"\r\n(The downloaded file is still at " + file + L")");
    }
    return 0;
}

static void CatLayout() {
    RECT rc; GetClientRect(g_cat, &rc);
    int W = rc.right, H = rc.bottom, m = S(10), rh = S(24), y = m;
    MoveWindow(g_catQ, m, y, W - 3 * m - S(90), rh, TRUE);
    MoveWindow(g_catGo, W - m - S(90), y, S(90), rh, TRUE);
    y += rh + m;
    int bottom = H - m;
    MoveWindow(g_catStat, m, bottom - rh, W - 2 * m, rh, TRUE); bottom -= rh + S(4);
    MoveWindow(g_catProg, m, bottom - S(18), W - 2 * m, S(18), TRUE); bottom -= S(18) + m;
    MoveWindow(g_catGet, W - m - S(110), bottom - rh, S(110), rh, TRUE);
    MoveWindow(g_catAuto, m, bottom - rh, W - 3 * m - S(110), rh, TRUE); bottom -= rh + m;
    MoveWindow(g_catList, m, y, W - 2 * m, std::max(S(60), bottom - y), TRUE);
    int lw = W - 2 * m - GetSystemMetrics(SM_CXVSCROLL) - 4;
    ListView_SetColumnWidth(g_catList, 0, lw * 62 / 100);
    ListView_SetColumnWidth(g_catList, 1, lw * 20 / 100);
    ListView_SetColumnWidth(g_catList, 2, lw * 18 / 100);
}

static void CatSetBusy(bool b) {
    g_catBusy = b;
    EnableWindow(g_catGo, !b); EnableWindow(g_catGet, !b); EnableWindow(g_catQ, !b);
    EnableWindow(g_catAuto, !b); EnableWindow(g_catList, !b);
}

static LRESULT CALLBACK CatWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        g_catQ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, hw, (HMENU)301, hi, nullptr);
        SendMessageW(g_catQ, EM_SETCUEBANNER, TRUE, (LPARAM)L"KB number or name, e.g. KB5034441");
        g_catGo = CreateWindowExW(0, L"BUTTON", L"Search", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hw, (HMENU)302, hi, nullptr);
        g_catList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 0, 0, hw, (HMENU)303, hi, nullptr);
        ListView_SetExtendedListViewStyle(g_catList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* cols[] = { L"Update", L"Date", L"Size" };
        for (int i = 0; i < 3; ++i) { LVCOLUMNW c = {}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = const_cast<LPWSTR>(cols[i]); c.cx = 100; ListView_InsertColumn(g_catList, i, &c); }
        g_catAuto = CreateWindowExW(0, L"BUTTON", L"Install automatically after download", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hw, (HMENU)304, hi, nullptr);
        SendMessageW(g_catAuto, BM_SETCHECK, RegGetU(L"CatAuto", 1) ? BST_CHECKED : BST_UNCHECKED, 0);
        g_catGet = CreateWindowExW(0, L"BUTTON", L"Get", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, hw, (HMENU)305, hi, nullptr);
        g_catProg = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hw, (HMENU)306, hi, nullptr);
        SendMessageW(g_catProg, PBM_SETRANGE32, 0, 1000);
        g_catStat = CreateWindowExW(0, L"STATIC", L"Type a KB number (or a name) and click Search.", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS, 0, 0, 0, 0, hw, (HMENU)307, hi, nullptr);
        EnumChildWindows(hw, SetFontProc, 0);
        CatLayout();
        return 0;
    }
    case WM_SIZE: if (g_catList) CatLayout(); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)lp)->ptMinTrackSize = { S(420), S(320) }; return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 302 && !g_catBusy) {
            wchar_t q[256]; GetWindowTextW(g_catQ, q, 256);
            if (!q[0]) { SetWindowTextW(g_catStat, L"Type a KB number or a name first."); return 0; }
            CatSetBusy(true);
            ListView_DeleteAllItems(g_catList); g_catRes.clear();
            auto* a = new SearchArgs{ WideToUtf8(q) };
            HANDLE t = CreateThread(nullptr, 0, CatSearchThread, a, 0, nullptr);
            if (t) CloseHandle(t); else { delete a; CatSetBusy(false); }
        } else if (LOWORD(wp) == 305 && !g_catBusy) {
            int i = ListView_GetNextItem(g_catList, -1, LVNI_SELECTED);
            if (i < 0 || i >= (int)g_catRes.size()) { SetWindowTextW(g_catStat, L"Pick an update from the list first."); return 0; }
            bool autoInst = SendMessageW(g_catAuto, BM_GETCHECK, 0, 0) == BST_CHECKED;
            RegPutU(L"CatAuto", autoInst ? 1 : 0);
            if (autoInst) {
                if (g_busy || !g_haveSession) { SetWindowTextW(g_catStat, L"The main window is busy. Wait for it to finish, then try again."); return 0; }
                if (MessageBoxW(hw, (L"Download and install this update?\r\n\r\n" + Utf8ToWide(g_catRes[i].title)).c_str(),
                                L"Windows Features", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 0;
                g_busy = true; g_catHoldsMain = true; UpdateControls();
            }
            CatSetBusy(true);
            auto* a = new GetArgs{ g_catRes[i].guid, g_catRes[i].title, autoInst };
            HANDLE t = CreateThread(nullptr, 0, CatGetThread, a, 0, nullptr);
            if (t) CloseHandle(t);
            else { delete a; CatSetBusy(false); if (g_catHoldsMain) { g_busy = false; g_catHoldsMain = false; UpdateControls(); } }
        }
        return 0;
    case WM_CAT_STATUS: { std::unique_ptr<std::wstring> s((std::wstring*)lp); SetWindowTextW(g_catStat, s->c_str()); return 0; }
    case WM_CAT_PROG: SendMessageW(g_catProg, PBM_SETPOS, wp, 0); return 0;
    case WM_CAT_RESULTS: {
        std::unique_ptr<std::vector<cat::Result>> r((std::vector<cat::Result>*)lp);
        g_catRes = *r;
        ListView_DeleteAllItems(g_catList);
        for (size_t i = 0; i < g_catRes.size(); ++i) {
            std::wstring t = Utf8ToWide(g_catRes[i].title), d = Utf8ToWide(g_catRes[i].date), s = Utf8ToWide(g_catRes[i].size);
            LVITEMW it = {}; it.mask = LVIF_TEXT; it.iItem = (int)i; it.pszText = &t[0];
            ListView_InsertItem(g_catList, &it);
            ListView_SetItemText(g_catList, (int)i, 1, &d[0]);
            ListView_SetItemText(g_catList, (int)i, 2, &s[0]);
        }
        if (!g_catRes.empty()) ListView_SetItemState(g_catList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        return 0;
    }
    case WM_CAT_DONE: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        int kind = (int)wp;
        CatSetBusy(false);
        SetWindowTextW(g_catStat, s->c_str());
        SetStatus(*s);
        if (kind != 0) SendMessageW(g_catProg, PBM_SETPOS, kind == 3 ? 0 : 1000, 0);
        bool held = g_catHoldsMain;
        if (held) { g_busy = false; g_catHoldsMain = false; UpdateControls(); }
        if (kind == 3) MessageBoxW(hw, s->c_str(), L"Windows Features", MB_OK | MB_ICONWARNING);
        else if (kind == 1) MessageBoxW(hw, s->c_str(), L"Windows Features", MB_OK | MB_ICONINFORMATION);
        else if (kind == 2 || kind == 4) {
            MessageBoxW(hw, s->c_str(), L"Windows Features", MB_OK | MB_ICONINFORMATION);
            if (kind == 4 && MessageBoxW(hw, L"A restart is required to finish. Restart now?", L"Windows Features", MB_YESNO | MB_ICONQUESTION) == IDYES) RebootNow();
            if (held) StartLoad();
        }
        return 0;
    }
    case WM_CLOSE:
        if (g_catBusy) { MessageBoxW(hw, L"Please wait for the current download or install to finish.", L"Windows Features", MB_OK); return 0; }
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY: g_cat = nullptr; return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void OpenCatalog() {
    if (g_cat) { SetForegroundWindow(g_cat); return; }
    g_catBusy = false;
    g_cat = CreateWindowExW(0, L"WFCatalog", L"Microsoft Update Catalog", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, S(720), S(480), g_wnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (g_cat) { ShowWindow(g_cat, SW_SHOW); SetFocus(g_catQ); }
}

// --- automatic repair (DISM RestoreHealth) ---
static void RepairChunk(const char* d, DWORD n, void*) {
    for (DWORD i = n; i-- > 0;) {
        if (d[i] != '%') continue;
        DWORD j = i;
        while (j > 0 && ((d[j - 1] >= '0' && d[j - 1] <= '9') || d[j - 1] == '.')) --j;
        if (j < i) {
            double v = atof(std::string(d + j, i - j).c_str());
            PostMessageW(g_wnd, WM_PROGRESS, (WPARAM)(v * 10), 0);
            PostStatus(L"Repairing Windows... " + std::to_wstring((int)v) + L"%");
        }
        break;
    }
}
static DWORD WINAPI RepairThread(LPVOID) {
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    std::string out;
    bool ran = RunCapture(L"\"" + std::wstring(sys) + L"\\dism.exe\" /Online /Cleanup-Image /RestoreHealth /NoRestart", out, RepairChunk, nullptr);
    PostMessageW(g_wnd, WM_REPAIRED, ran && (g_lastExit == 0 || g_lastExit == 3010), 0);
    return 0;
}
static void StartRepair() {
    g_busy = true;
    UpdateControls();
    StopMarquee();
    SetStatus(L"Repairing Windows... 0%");
    HANDLE h = CreateThread(nullptr, 0, RepairThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else { g_busy = false; UpdateControls(); }
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    HMENU ts = CreatePopupMenu();
    AppendMenuW(ts, MF_STRING | (g_textPct == 100 ? MF_CHECKED : 0), IDM_TEXT_100, L"Normal");
    AppendMenuW(ts, MF_STRING | (g_textPct == 125 ? MF_CHECKED : 0), IDM_TEXT_125, L"Large");
    AppendMenuW(ts, MF_STRING | (g_textPct == 150 ? MF_CHECKED : 0), IDM_TEXT_150, L"Extra large");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ts, L"Text and box size (Ctrl + / Ctrl -)");
    AppendMenuW(m, MF_STRING | (g_beginner ? MF_CHECKED : 0), IDM_BEGINNER, L"Beginner help (plain-English tips)");
    AppendMenuW(m, MF_STRING, IDM_GUIDE, L"Quick guide");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_CATALOG, L"Get an update from Microsoft Update Catalog...");
    AppendMenuW(m, MF_STRING, IDM_REPAIR, L"Repair Windows automatically...");
    AppendMenuW(m, MF_STRING | (g_autoFix ? MF_CHECKED : 0), IDM_AUTOFIX, L"Fix missing files automatically when a feature fails");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_EXPAND, L"Expand all");
    AppendMenuW(m, MF_STRING, IDM_COLLAPSE, L"Collapse all");
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"Refresh");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    bool on = IfeoIsSet();
    AppendMenuW(m, MF_STRING, IDM_SETDEFAULT, on ? L"Update the installed default app (copy this version)" : L"Use as the default Windows features app (replace optionalfeatures.exe)");
    AppendMenuW(m, MF_STRING | (on ? 0 : MF_GRAYED), IDM_RESTORE, L"Restore the original Windows app");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_ABOUT, L"About");
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    RECT rc; GetWindowRect(g_menuBtn, &rc);
    TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_TOPALIGN, rc.right, rc.bottom, 0, g_wnd, nullptr);
    DestroyMenu(m);
}

static void DoRefresh() {
    if (g_busy) return;
    if (PendingCount() && MessageBoxW(g_wnd, L"Discard unapplied changes?", L"Windows Features", MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
    StartLoad();
}

static LRESULT CALLBACK WndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_wnd = hw;
        typedef UINT (WINAPI *PGetDpi)(HWND);
        auto gd = (PGetDpi)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        g_dpi = gd ? (int)gd(hw) : 96;

        g_tab = Mk(WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS, 0, ID_TAB);
        TabCtrl_SetPadding(g_tab, S(16), S(7));
        SetWindowSubclass(g_tab, TabProc, 1, 0);
        g_menuBtn = Mk(L"BUTTON", L"Menu \u25BE", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_MENUBTN);
        g_search = Mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_SEARCH);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search by name or description...");
        g_filter = Mk(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 0, ID_FILTER);
        SendMessageW(g_filter, CB_ADDSTRING, 0, (LPARAM)L"Show: all");
        SendMessageW(g_filter, CB_ADDSTRING, 0, (LPARAM)L"Show: turned on");
        SendMessageW(g_filter, CB_ADDSTRING, 0, (LPARAM)L"Show: changed");
        SendMessageW(g_filter, CB_SETCURSEL, 0, 0);
        g_expand = Mk(L"BUTTON", L"Expand all", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_EXPAND);
        g_collapse = Mk(L"BUTTON", L"Collapse all", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_COLLAPSE);

        g_tree = Mk(WC_TREEVIEWW, L"", WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT | TVS_CHECKBOXES |
                    TVS_SHOWSELALWAYS | TVS_FULLROWSELECT | TVS_NOHSCROLL | TVS_INFOTIP, WS_EX_CLIENTEDGE, ID_TREE);
        SendMessageW(g_tree, TVM_SETEXTENDEDSTYLE, 0, TVS_EX_DOUBLEBUFFER | TVS_EX_PARTIALCHECKBOX | TVS_EX_FADEINOUTEXPANDOS);

        g_dtitle = Mk(L"STATIC", L"", SS_ENDELLIPSIS, 0, ID_DTITLE);
        g_desc = Mk(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL, WS_EX_CLIENTEDGE, ID_DESC);
        g_srcLbl = Mk(L"STATIC", L"Source path:", 0, 0, ID_SRCLBL);
        g_src = Mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, ID_SRC);
        SendMessageW(g_src, EM_SETCUEBANNER, TRUE, (LPARAM)L"Optional: folder with the payload (e.g. D:\\sources\\sxs)");
        g_browse = Mk(L"BUTTON", L"Find...", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_BROWSE);
        g_payload = Mk(L"BUTTON", L"Remove payload when turning features off (frees disk space)", WS_TABSTOP | BS_AUTOCHECKBOX, 0, ID_PAYLOAD);
        ShowWindow(g_payload, g_isServer ? SW_SHOW : SW_HIDE);
        g_prog = Mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, 0, ID_PROG);
        g_status = Mk(L"STATIC", L"", SS_ENDELLIPSIS, 0, ID_STATUS);
        g_refresh = Mk(L"BUTTON", L"Refresh", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_REFRESH);
        g_apply = Mk(L"BUTTON", L"Apply (0)", WS_TABSTOP | BS_PUSHBUTTON, 0, ID_APPLY);
        MakeFonts();
        g_tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               0, 0, 0, 0, hw, nullptr, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_tt, TTM_SETMAXTIPWIDTH, 0, S(360));
        SendMessageW(g_tt, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
        AddTip(g_search, L"Type part of a name or description to narrow the list.");
        AddTip(g_filter, L"Show everything, only what is turned on right now, or only the changes you have made but not applied yet.");
        AddTip(g_expand, L"Open every group so you can see all the items inside.");
        AddTip(g_collapse, L"Close all groups.");
        AddTip(g_src, L"Only needed if Windows cannot download the files it needs (an offline PC, or a server where the files were removed). Point this at the 'sources\\sxs' folder of a Windows install disc or ISO.");
        AddTip(g_browse, L"Find the files Windows needs: search your drives for a mounted Windows ISO or disc, pick an ISO file (I will mount it), or choose a folder. The media must match your version of Windows.");
        AddTip(g_payload, L"Deletes the installation files when you turn a feature off, to save disk space. You will need a source to turn it on again later.");
        AddTip(g_refresh, L"Reload the list from Windows. Changes you have not applied are discarded.");
        AddTip(g_apply, L"Nothing changes on your PC until you click Apply. The number shows how many changes are waiting.");
        AddTip(g_menuBtn, L"Beginner help, quick guide and other options.");
        ApplyTheme();
        Layout();
        StartLoad();
        return 0;
    }
    case WM_SIZE:
        if (g_tree) Layout();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        ApplyTheme();
        RECT* r = (RECT*)lp;
        SetWindowPos(hw, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        MakeFonts();
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize.x = S(600);
        ((MINMAXINFO*)lp)->ptMinTrackSize.y = S(520);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC dc = (HDC)wp;
        HWND c = (HWND)lp;
        SetTextColor(dc, g_cText);
        if (c == g_desc) { SetBkColor(dc, g_cCtl); return (LRESULT)g_brCtl; }
        POINT o = { 0, 0 };
        MapWindowPoints(c, hw, &o, 1);
        SetBrushOrgEx(dc, -o.x, -o.y, nullptr);
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)g_brBg;
    }
    case WM_ERASEBKGND: {
        HDC dc = (HDC)wp;
        RECT rc; GetClientRect(hw, &rc);
        POINT o = { 0, 0 };
        GetViewportOrgEx(dc, &o);
        SetBrushOrgEx(dc, o.x, o.y, nullptr);
        FillRect(dc, &rc, g_brBg);
        return 1;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SEARCH: if (HIWORD(wp) == EN_CHANGE && !g_busy) Populate(); break;
        case ID_FILTER: if (HIWORD(wp) == CBN_SELCHANGE && !g_busy) Populate(); break;
        case ID_EXPAND: case IDM_EXPAND:     ExpandAll(TreeView_GetRoot(g_tree), TVE_EXPAND); break;
        case ID_COLLAPSE: case IDM_COLLAPSE: ExpandAll(TreeView_GetRoot(g_tree), TVE_COLLAPSE); break;
        case ID_REFRESH: case IDM_REFRESH:   DoRefresh(); break;
        case ID_APPLY:   if (!g_busy) { g_repairTried = false; DoApply(); } break;
        case ID_BROWSE:  ShowFindMenu(); break;
        case ID_MENUBTN: ShowMenu(); break;
        case IDM_BEGINNER:
            g_beginner = !g_beginner;
            RegPutU(L"Beginner", g_beginner ? 1 : 0);
            break;
        case IDM_GUIDE: ShowGuide(); break;
        case IDM_CATALOG: OpenCatalog(); break;
        case IDM_AUTOFIX:
            g_autoFix = !g_autoFix;
            RegPutU(L"AutoFix", g_autoFix ? 1 : 0);
            break;
        case IDM_REPAIR:
            if (g_busy) break;
            if (MessageBoxW(hw, L"Check Windows for damaged files and repair them (uses Windows Update)?\r\n\r\nThis can take 10 minutes or more.",
                            L"Windows Features", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) StartRepair();
            break;
        case IDM_TEXT_100: case IDM_TEXT_125: case IDM_TEXT_150:
            g_textPct = LOWORD(wp) == IDM_TEXT_100 ? 100 : LOWORD(wp) == IDM_TEXT_125 ? 125 : 150;
            RegPutU(L"TextPct", g_textPct);
            ApplyTextSize();
            break;
        case IDM_SETDEFAULT: {
            std::wstring m;
            if (MessageBoxW(hw, L"Make WinFeatures open whenever Windows launches optionalfeatures.exe "
                                L"(Settings > More Windows features, Win+R, Control Panel)?\r\n\r\n"
                                L"The original file is not modified; you can undo this from the File menu.",
                            L"Windows Features", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
            bool ok = IfeoInstall(m);
            MessageBoxW(hw, m.c_str(), L"Windows Features", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
            break;
        }
        case IDM_RESTORE: {
            std::wstring m; IfeoRemove(m);
            MessageBoxW(hw, m.c_str(), L"Windows Features", MB_OK | MB_ICONINFORMATION);
            break;
        }
        case IDM_ABOUT:
            MessageBoxW(hw, (std::wstring(L"Build: " WSTR(__DATE__) L" " WSTR(__TIME__) L"\r\n\r\n") + (g_isServer ? L"WinFeatures - native replacement for Optional Features and Server Manager's "
                                         L"Add Roles and Features (local server)."
                                       : L"WinFeatures - native replacement for \"Turn Windows features on or off\" and Optional features.")).c_str(),
                        L"About", MB_OK | MB_ICONINFORMATION);
            break;
        case IDM_EXIT: SendMessageW(hw, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_NOTIFY: {
        auto* nh = (NMHDR*)lp;
        if (nh->idFrom == ID_TAB && nh->code == TCN_SELCHANGE) {
            g_cur = TabCtrl_GetCurSel(g_tab);
            Populate();
        } else if (nh->idFrom == ID_TREE) {
            if (nh->code == TVN_SELCHANGEDW) ShowDescription((int)((NMTREEVIEWW*)lp)->itemNew.lParam);
            else if (nh->code == NM_CUSTOMDRAW) {
                auto* cd = (NMTVCUSTOMDRAW*)lp;
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return (g_cur == g_allTab) ? CDRF_NOTIFYITEMDRAW : CDRF_DODEFAULT;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    int i = (int)cd->nmcd.lItemlParam;
                    if (i >= 0 && i < (int)g_nodes.size()) {
                        COLORREF c = CatColor(g_nodes[i]);
                        if (c != (COLORREF)CLR_DEFAULT) cd->clrText = c;
                    }
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
            }
            else if (nh->code == NM_CLICK) {
                DWORD pos = GetMessagePos();
                TVHITTESTINFO ht = {};
                ht.pt.x = (short)LOWORD(pos); ht.pt.y = (short)HIWORD(pos);
                ScreenToClient(g_tree, &ht.pt);
                HTREEITEM hit = TreeView_HitTest(g_tree, &ht);
                if (hit && (ht.flags & TVHT_ONITEMSTATEICON)) PostMessageW(hw, WM_TOGGLE, 0, (LPARAM)hit);
            } else if (nh->code == TVN_GETINFOTIPW) {
                if (g_beginner) {
                    auto* gi = (NMTVGETINFOTIPW*)lp;
                    std::wstring t = TipText((int)gi->lParam);
                    if (gi->cchTextMax > 0) {
                        wcsncpy(gi->pszText, t.c_str(), gi->cchTextMax - 1);
                        gi->pszText[gi->cchTextMax - 1] = 0;
                    }
                }
            } else if (nh->code == TVN_KEYDOWN) {
                if (((NMTVKEYDOWN*)lp)->wVKey == VK_SPACE) {
                    HTREEITEM sel = TreeView_GetSelection(g_tree);
                    if (sel) PostMessageW(hw, WM_TOGGLE, 0, (LPARAM)sel);
                }
            }
        }
        return 0;
    }
    case WM_TOGGLE:
        Toggle((HTREEITEM)lp);
        return 0;
    case WM_REPAIRED: {
        g_busy = false;
        bool wasRetry = g_retryPending;
        if (!wp) {
            g_retry.clear(); g_retryPending = false;
            MessageBoxW(hw, L"The automatic repair didn't finish. You can try again, or use the Source path with install media.",
                        L"Windows Features", MB_OK | MB_ICONWARNING);
        } else if (!wasRetry) {
            MessageBoxW(hw, L"Repair finished.", L"Windows Features", MB_OK | MB_ICONINFORMATION);
        }
        StartLoad();
        return 0;
    }
    case WM_STATUSMSG: {
        std::unique_ptr<std::wstring> s((std::wstring*)lp);
        SetStatus(*s);
        return 0;
    }
    case WM_LOADED: {
        std::unique_ptr<LoadResult> r((LoadResult*)lp);
        g_nodes = std::move(r->nodes);
        RebuildTile();
        r->tabs.push_back(L"Everything");
        g_allTab = (int)r->tabs.size() - 1;
        BuildSuggestions();
        if (!g_sugIdx.empty()) { r->tabs.push_back(L"Suggestions"); g_sugTab = (int)r->tabs.size() - 1; }
        else g_sugTab = -1;
        TabCtrl_DeleteAllItems(g_tab);
        for (size_t i = 0; i < r->tabs.size(); ++i) {
            TCITEMW ti = {}; ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<LPWSTR>(r->tabs[i].c_str());
            TabCtrl_InsertItem(g_tab, (int)i, &ti);
        }
        if (g_cur >= (int)r->tabs.size()) g_cur = 0;
        TabCtrl_SetCurSel(g_tab, g_cur);
        g_busy = false;
        StopMarquee();
        Populate();
        if (g_retryPending) {
            g_retryPending = false;
            for (auto& rn : g_retry) for (auto& n : g_nodes) if (n.name == rn.name && n.orig != rn.enable) n.want = rn.enable;
            g_retry.clear();
            RefreshChecks();
            UpdateControls();
            if (PendingCount()) DoApply(true);
        }
        if (RegGetU(L"GuideShown", 0) == 0) { RegPutU(L"GuideShown", 1); ShowGuide(); }
        if (!r->err.empty()) MessageBoxW(hw, r->err.c_str(), L"Windows Features", MB_OK | MB_ICONWARNING);
        return 0;
    }
    case WM_PROGRESS:
        SendMessageW(g_prog, PBM_SETPOS, wp, 0);
        return 0;
    case WM_APPLIED: {
        std::unique_ptr<ApplyResult> r((ApplyResult*)lp);
        std::wstring m = L"Done: " + std::to_wstring(r->ok) + L" succeeded, " + std::to_wstring(r->failed) + L" failed.";
        if (!r->errors.empty()) m += L"\r\n\r\n" + r->errors;
        MessageBoxW(hw, m.c_str(), L"Windows Features", MB_OK | (r->failed ? MB_ICONWARNING : MB_ICONINFORMATION));
        bool reboot = r->reboot;
        bool startedRepair = false;
        if (r->sourceProblem) {
            wchar_t cur[MAX_PATH] = {};
            GetWindowTextW(g_src, cur, MAX_PATH);
            if (cur[0]) {
                MessageBoxW(hw, L"Windows still couldn't find the files, even with the Source path set.\r\n\r\n"
                                L"Make sure the install media matches your version of Windows (same build and language), then try again.",
                            L"Windows Features", MB_OK | MB_ICONWARNING);
            } else {
                bool preferFod = false;
                for (auto& fl : r->fails)
                    for (auto& n : g_nodes) if (n.name == fl.first && n.kind == K_DISM_CAPABILITY) preferFod = true;
                auto media = FindMedia(preferFod);
                auto queueRetry = [&]() {
                    g_retry.clear();
                    for (auto& fl : r->fails) if (fl.second) g_retry.push_back({ fl.first, true });
                    g_retryPending = !g_retry.empty();
                };
                if (g_autoFix && !media.empty()) {
                    SetWindowTextW(g_src, media[0].c_str());
                    queueRetry();
                } else if (g_autoFix && !g_repairTried) {
                    g_repairTried = true;
                    queueRetry();
                    startedRepair = true;
                    StartRepair();
                } else if (!media.empty()) {
                    std::wstring q = L"Windows couldn't get the files it needs (Windows Update didn't supply them).\r\n\r\n"
                                     L"I found install media here:\r\n    " + MediaLabel(media[0]) +
                                     L"\r\n\r\nUse it and try again? (It must match your version of Windows.)";
                    if (MessageBoxW(hw, q.c_str(), L"Windows Features", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        SetWindowTextW(g_src, media[0].c_str());
                        g_retry.clear();
                        for (auto& fl : r->fails) if (fl.second) g_retry.push_back({ fl.first, true });
                        g_retryPending = !g_retry.empty();
                    }
                } else {
                    MessageBoxW(hw, L"No install media was found.\r\n\r\nMount a Windows ISO that matches your version (or insert the disc), "
                                    L"then click Find... next to Source path and choose 'Search my drives for install media'. Then click Apply again.",
                                L"Windows Features", MB_OK | MB_ICONINFORMATION);
                }
            }
        }
        if (!startedRepair) StartLoad();
        if (reboot && MessageBoxW(hw, L"A restart is required to finish. Restart now?", L"Windows Features",
                                  MB_YESNO | MB_ICONQUESTION) == IDYES)
            RebootNow();
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) { MessageBoxW(hw, L"Please wait for the current operation to finish.", L"Windows Features", MB_OK); return 0; }
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        if (g_haveSession) g_api.CloseSession(g_session);
        if (g_initialized) g_api.Shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static void SetTextPct(int pct) {
    pct = std::max(70, std::min(220, pct));
    if (pct == g_textPct) return;
    g_textPct = pct;
    RegPutU(L"TextPct", pct);
    ApplyTextSize();
    SetStatus(L"Text and box size " + std::to_wstring(pct) + L"%");
}

// Ctrl + / Ctrl - / Ctrl 0 and Ctrl + mouse wheel resize the text and boxes.
static bool HandleZoomKeys(const MSG& m) {
    if (!(GetKeyState(VK_CONTROL) & 0x8000)) return false;
    if (m.message == WM_KEYDOWN) {
        switch (m.wParam) {
        case VK_OEM_PLUS: case VK_ADD:        SetTextPct(g_textPct + 10); return true;
        case VK_OEM_MINUS: case VK_SUBTRACT:  SetTextPct(g_textPct - 10); return true;
        case '0': case VK_NUMPAD0:            SetTextPct(100); return true;
        }
    } else if (m.message == WM_MOUSEWHEEL) {
        SetTextPct(g_textPct + (GET_WHEEL_DELTA_WPARAM(m.wParam) > 0 ? 10 : -10));
        return true;
    }
    return false;
}

static bool IsElevated() {
    bool r = false;
    HANDLE t;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
        TOKEN_ELEVATION e; DWORD sz;
        if (GetTokenInformation(t, TokenElevation, &e, sizeof(e), &sz)) r = e.TokenIsElevated != 0;
        CloseHandle(t);
    }
    return r;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR args, int show) {
    // The manifest already requests administrator rights; this is a safety net if it was stripped.
    if (!IsElevated()) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        if ((INT_PTR)ShellExecuteW(nullptr, L"runas", self, args, nullptr, show) > 32) return 0;
        MessageBoxW(nullptr, L"Administrator rights are needed to change Windows features.", L"Windows Features", MB_OK | MB_ICONWARNING);
        return 1;
    }
    std::wstring cl = Lower(GetCommandLineW());
    bool silent = cl.find(L"--silent") != std::wstring::npos;
    if (cl.find(L"--install") != std::wstring::npos || cl.find(L"--uninstall") != std::wstring::npos) {
        std::wstring m;
        bool ok = cl.find(L"--uninstall") != std::wstring::npos ? IfeoRemove(m) : IfeoInstall(m);
        if (!silent) MessageBoxW(nullptr, m.c_str(), L"Windows Features", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONERROR));
        return ok ? 0 : 1;
    }
    g_isServer = DetectServer();
    if (cl.find(L"--server") != std::wstring::npos) g_isServer = true;
    if (cl.find(L"--client") != std::wstring::npos) g_isServer = false;

    g_beginner = RegGetU(L"Beginner", 1) != 0;
    g_autoFix = RegGetU(L"AutoFix", 0) != 0;
    g_textPct = (int)RegGetU(L"TextPct", 100);
    if (g_textPct < 70 || g_textPct > 220) g_textPct = 100;
    InitUx();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TAB_CLASSES | ICC_TREEVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"WinFeaturesWnd";
    RegisterClassW(&wc);
    WNDCLASSW cc = wc;
    cc.lpfnWndProc = CatWndProc;
    cc.lpszClassName = L"WFCatalog";
    RegisterClassW(&cc);

    HWND w = CreateWindowExW(0, wc.lpszClassName, g_isServer ? L"Roles and Features" : L"Windows Features",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 820, 760, nullptr, nullptr, hi, nullptr);
    SendMessageW(w, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    ShowWindow(w, show);
    UpdateWindow(w);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (HandleZoomKeys(m)) continue;
        if (!IsDialogMessageW(w, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return (int)m.wParam;
}
