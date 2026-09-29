// symfetch - build Microsoft Symbol Server URLs from a PE file (or from name/timestamp/size)
// and optionally download the binary or its PDB from the public server (msdl.microsoft.com).
//
// C++17, no third-party dependencies. The download part uses WinHTTP (part of Windows).
// The PE parsing is portable, so the URL logic can be tested on any OS (-DSYMFETCH_NO_NET).
//
// Usage:
//   symfetch <pe_file>                       print the URL of the binary itself
//   symfetch <pe_file> --pdb                 print the URL of the matching .pdb
//   symfetch <pe_file> --download [out]      download the binary (default: ./<name>)
//   symfetch <pe_file> --pdb --download      download the .pdb
//   symfetch --id <name> <timestamp_hex> <size_hex> [--download [out]]
//                                            for a file you do not have locally
//
// Only files Microsoft has published to its symbol server are available; the server
// returns 404 for anything else. It serves binaries and symbols, not source code.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32) && !defined(SYMFETCH_NO_NET)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
#define HAVE_NET 1
#endif

static const char* kServer = "https://msdl.microsoft.com/download/symbols/";
[[maybe_unused]] static const wchar_t* kUserAgent = L"Microsoft-Symbol-Server/10.0.0.0";

// ---------------------------------------------------------------------------
// Minimal PE reader (little-endian, bounds-checked)
// ---------------------------------------------------------------------------
struct Pe {
    std::vector<uint8_t> d;
    uint32_t timeDateStamp = 0, sizeOfImage = 0;
    bool pe32plus = false;
    uint32_t optOff = 0, dirOff = 0;
    uint16_t nSections = 0;
    uint32_t secOff = 0;

    bool ok(size_t off, size_t len) const { return off <= d.size() && len <= d.size() - off; }
    uint16_t u16(size_t o) const { return (uint16_t)(d[o] | d[o + 1] << 8); }
    uint32_t u32(size_t o) const { return (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; }

    bool parse(std::string& err) {
        if (!ok(0, 0x40) || d[0] != 'M' || d[1] != 'Z') { err = "not a PE file (no MZ header)"; return false; }
        uint32_t pe = u32(0x3C);
        if (!ok(pe, 24) || std::memcmp(&d[pe], "PE\0\0", 4) != 0) { err = "not a PE file (no PE signature)"; return false; }
        nSections = u16(pe + 6);
        timeDateStamp = u32(pe + 8);
        uint16_t optSize = u16(pe + 20);
        optOff = pe + 24;
        if (!ok(optOff, optSize) || optSize < 96) { err = "truncated optional header"; return false; }
        uint16_t magic = u16(optOff);
        if (magic == 0x10B) pe32plus = false;
        else if (magic == 0x20B) pe32plus = true;
        else { err = "unknown optional header magic"; return false; }
        sizeOfImage = u32(optOff + 56);
        dirOff = optOff + (pe32plus ? 112 : 96);
        secOff = optOff + optSize;
        if (!ok(secOff, (size_t)nSections * 40)) { err = "truncated section table"; return false; }
        return true;
    }

    bool rvaToOff(uint32_t rva, uint32_t& off) const {
        for (uint16_t i = 0; i < nSections; ++i) {
            size_t s = secOff + (size_t)i * 40;
            uint32_t vsize = u32(s + 8), va = u32(s + 12), rawSize = u32(s + 16), raw = u32(s + 20);
            uint32_t span = vsize > rawSize ? vsize : rawSize;
            if (rva >= va && rva - va < span) { off = raw + (rva - va); return true; }
        }
        return false;
    }

    // Finds the CodeView (RSDS) record: PDB name and key "GUID(32 hex)Age(hex)".
    bool pdbInfo(std::string& pdbName, std::string& key, std::string& err) const {
        if (!ok(dirOff, 8 * 8)) { err = "no data directories"; return false; }
        uint32_t nDirs = u32(optOff + (pe32plus ? 108 : 92));
        if (nDirs <= 6) { err = "file has no debug directory"; return false; }
        uint32_t dbgRva = u32(dirOff + 6 * 8), dbgSize = u32(dirOff + 6 * 8 + 4), dbgOff;
        if (!dbgRva || !rvaToOff(dbgRva, dbgOff)) { err = "file has no debug directory"; return false; }
        for (uint32_t i = 0; i + 28 <= dbgSize; i += 28) {
            size_t e = dbgOff + i;
            if (!ok(e, 28)) break;
            if (u32(e + 12) != 2) continue;                       // IMAGE_DEBUG_TYPE_CODEVIEW
            uint32_t size = u32(e + 16), raw = u32(e + 24);
            if (size < 24 || !ok(raw, size) || std::memcmp(&d[raw], "RSDS", 4) != 0) continue;
            char buf[64];
            uint32_t d1 = u32(raw + 4);
            uint16_t d2 = u16(raw + 8), d3 = u16(raw + 10);
            std::snprintf(buf, sizeof(buf), "%08X%04X%04X", d1, d2, d3);
            key = buf;
            for (int b = 0; b < 8; ++b) { std::snprintf(buf, sizeof(buf), "%02X", d[raw + 12 + b]); key += buf; }
            std::snprintf(buf, sizeof(buf), "%X", u32(raw + 20));  // Age, no leading zeros
            key += buf;
            std::string path((const char*)&d[raw + 24], strnlen((const char*)&d[raw + 24], size - 24));
            size_t slash = path.find_last_of("\\/");
            pdbName = slash == std::string::npos ? path : path.substr(slash + 1);
            if (pdbName.empty()) { err = "the debug record has no PDB name"; return false; }
            return true;
        }
        err = "no CodeView/PDB record in the debug directory";
        return false;
    }
};

static std::string Basename(const std::string& p) {
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// https://msdl.microsoft.com/download/symbols/<name>/<TIMESTAMP 8 hex><SIZEOFIMAGE hex>/<name>
static std::string BinaryUrl(const std::string& name, uint32_t stamp, uint32_t size) {
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%08X%X", stamp, size);
    return std::string(kServer) + name + "/" + hex + "/" + name;
}

// ---------------------------------------------------------------------------
// Download (WinHTTP)
// ---------------------------------------------------------------------------
#ifdef HAVE_NET
static bool Download(const std::string& url, const std::string& outPath, std::string& err) {
    int wl = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    std::wstring wurl(wl, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, &wurl[0], wl);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { err = "bad URL"; return false; }

    HINTERNET ses = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!ses) { err = "WinHttpOpen failed"; return false; }
    HINTERNET con = WinHttpConnect(ses, host, uc.nPort, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                              uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            std::ofstream out(outPath, std::ios::binary);
            if (!out) err = "cannot write " + outPath;
            else {
                std::vector<char> buf(64 * 1024);
                DWORD n = 0;
                unsigned long long total = 0;
                while (WinHttpReadData(req, buf.data(), (DWORD)buf.size(), &n) && n) { out.write(buf.data(), n); total += n; }
                ok = total > 0;
                if (!ok) err = "empty response";
                else std::printf("Saved %llu bytes to %s\n", total, outPath.c_str());
            }
        } else if (status == 404) err = "404: Microsoft has not published this file to the symbol server";
        else err = "HTTP status " + std::to_string(status);
    } else err = "request failed (error " + std::to_string(GetLastError()) + ")";
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
}
#else
static bool Download(const std::string&, const std::string&, std::string& err) {
    err = "download is only available in the Windows build";
    return false;
}
#endif

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    bool wantPdb = false, wantDownload = false;
    std::string outPath, target, idName;
    uint32_t idStamp = 0, idSize = 0;
    bool useId = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--pdb") wantPdb = true;
        else if (a == "--download") {
            wantDownload = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') outPath = argv[++i];
        } else if (a == "--id" && i + 3 < argc) {
            useId = true;
            idName = argv[++i];
            idStamp = (uint32_t)std::strtoul(argv[++i], nullptr, 16);
            idSize = (uint32_t)std::strtoul(argv[++i], nullptr, 16);
        } else if (target.empty() && a[0] != '-') target = a;
        else { std::fprintf(stderr, "Unknown argument: %s\n", a.c_str()); return 1; }
    }
    if (target.empty() && !useId) {
        std::fprintf(stderr,
            "Usage:\n  %s <pe_file> [--pdb] [--download [out]]\n  %s --id <name> <timestamp_hex> <size_hex> [--download [out]]\n",
            argv[0], argv[0]);
        return 1;
    }

    std::string url, fname;
    if (useId) {
        fname = idName;
        url = BinaryUrl(idName, idStamp, idSize);
    } else {
        std::ifstream f(target, std::ios::binary);
        if (!f) { std::fprintf(stderr, "Error: cannot open %s\n", target.c_str()); return 1; }
        Pe pe;
        pe.d.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        std::string err;
        if (!pe.parse(err)) { std::fprintf(stderr, "Error: %s\n", err.c_str()); return 1; }
        if (wantPdb) {
            std::string pdb, key;
            if (!pe.pdbInfo(pdb, key, err)) { std::fprintf(stderr, "Error: %s\n", err.c_str()); return 1; }
            fname = pdb;
            url = std::string(kServer) + pdb + "/" + key + "/" + pdb;
        } else {
            fname = Basename(target);
            url = BinaryUrl(fname, pe.timeDateStamp, pe.sizeOfImage);
        }
    }

    std::printf("%s\n", url.c_str());
    if (wantDownload) {
        std::string err;
        if (outPath.empty()) outPath = fname;
        if (!Download(url, outPath, err)) { std::fprintf(stderr, "Error: %s\n", err.c_str()); return 2; }
    }
    return 0;
}
