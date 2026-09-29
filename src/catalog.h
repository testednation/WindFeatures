// catalog.h - portable helpers for the Microsoft Update Catalog (no Windows headers).
//
// Flow used by the catalog website:
//   1. GET  https://www.catalog.update.microsoft.com/Search.aspx?q=<text or KB number>
//        -> HTML table; each row has  goToDetails("<update GUID>")  and the cells
//           Title | Products | Classification | Last Updated | Version | Size
//   2. POST https://www.catalog.update.microsoft.com/DownloadDialog.aspx
//           updateIDs=[{"size":0,"languages":"","uidInfo":"<GUID>","updateID":"<GUID>"}]
//        -> JavaScript containing   downloadInformation[0].files[0].url = '<direct URL>';
//
// Everything here is string handling so it can be unit-tested on any OS.
#pragma once
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace cat {

struct Result {
    std::string guid, title, products, classification, date, version, size;
};

inline std::string DecodeEntities(std::string s) {
    static const char* pairs[][2] = { { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" },
                                      { "&#39;", "'" }, { "&apos;", "'" }, { "&nbsp;", " " } };
    for (auto& p : pairs) {
        size_t pos = 0;
        while ((pos = s.find(p[0], pos)) != std::string::npos) { s.replace(pos, std::string(p[0]).size(), p[1]); pos += std::string(p[1]).size(); }
    }
    return s;
}

// Removes tags, decodes entities, collapses whitespace.
inline std::string StripTags(const std::string& in) {
    std::string out;
    bool tag = false;
    for (char c : in) {
        if (c == '<') tag = true;
        else if (c == '>') { tag = false; out += ' '; }
        else if (!tag) out += c;
    }
    out = DecodeEntities(out);
    std::string r;
    bool sp = true;
    for (char c : out) {
        bool w = std::isspace((unsigned char)c) != 0;
        if (w) { if (!sp) r += ' '; sp = true; }
        else { r += c; sp = false; }
    }
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}

inline bool LooksLikeGuid(const std::string& g) {
    if (g.size() != 36) return false;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (g[i] != '-') return false; }
        else if (!std::isxdigit((unsigned char)g[i])) return false;
    }
    return true;
}

inline std::vector<Result> ParseSearch(const std::string& html) {
    std::vector<Result> out;
    std::set<std::string> seen;
    size_t pos = 0;
    while ((pos = html.find("goToDetails(", pos)) != std::string::npos) {
        size_t q = html.find_first_of("\"'", pos);
        if (q == std::string::npos) break;
        size_t q2 = html.find(html[q], q + 1);
        if (q2 == std::string::npos) break;
        std::string guid = html.substr(q + 1, q2 - q - 1);
        pos = q2;
        if (!LooksLikeGuid(guid) || !seen.insert(guid).second) continue;

        size_t gt = html.find('>', q2);
        size_t endA = gt == std::string::npos ? gt : html.find("</a>", gt);
        if (endA == std::string::npos) break;
        Result r;
        r.guid = guid;
        r.title = StripTags(html.substr(gt + 1, endA - gt - 1));

        size_t rowEnd = html.find("<tr", endA);
        if (rowEnd == std::string::npos) rowEnd = html.size();
        std::vector<std::string> cells;
        size_t c = endA;
        while ((c = html.find("<td", c)) != std::string::npos && c < rowEnd) {
            size_t tdEnd = html.find('>', c);
            size_t close = tdEnd == std::string::npos ? tdEnd : html.find("</td>", tdEnd);
            if (close == std::string::npos || close > rowEnd) break;
            std::string raw = html.substr(tdEnd + 1, close - tdEnd - 1);
            size_t sp = raw.find("<span");
            if (sp != std::string::npos) {                       // size cell: first span is the readable size
                size_t se = raw.find("</span>", sp);
                if (se != std::string::npos) raw = raw.substr(sp, se - sp);
            }
            cells.push_back(StripTags(raw));
            c = close + 5;
        }
        if (cells.size() > 0) r.products = cells[0];
        if (cells.size() > 1) r.classification = cells[1];
        if (cells.size() > 2) r.date = cells[2];
        if (cells.size() > 3) r.version = cells[3];
        if (cells.size() > 4) r.size = cells[4];
        out.push_back(r);
    }
    return out;
}

// Finds every   .url = '...'   (single or double quotes) in the DownloadDialog response.
inline std::vector<std::string> ParseDownloadUrls(const std::string& js) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    size_t pos = 0;
    while ((pos = js.find(".url", pos)) != std::string::npos) {
        pos += 4;
        size_t p = pos;
        while (p < js.size() && (js[p] == ' ' || js[p] == '\t')) ++p;
        if (p >= js.size() || js[p] != '=') continue;
        ++p;
        while (p < js.size() && (js[p] == ' ' || js[p] == '\t')) ++p;
        if (p >= js.size() || (js[p] != '\'' && js[p] != '"')) continue;
        size_t e = js.find(js[p], p + 1);
        if (e == std::string::npos) break;
        std::string url = js.substr(p + 1, e - p - 1);
        pos = e;
        if (!url.empty() && seen.insert(url).second) out.push_back(url);
    }
    return out;
}

inline std::string HostOf(const std::string& url) {
    size_t s = url.find("://");
    if (s == std::string::npos) return "";
    s += 3;
    size_t e = url.find_first_of("/:?#", s);
    std::string h = url.substr(s, e == std::string::npos ? std::string::npos : e - s);
    for (auto& c : h) c = (char)std::tolower((unsigned char)c);
    return h;
}

inline bool EndsWith(const std::string& s, const std::string& suf) {
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

// Only follow direct links that point at Microsoft's update delivery hosts.
inline bool HostAllowed(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0 && url.compare(0, 7, "http://") != 0) return false;
    std::string h = HostOf(url);
    return h == "download.windowsupdate.com" || EndsWith(h, ".windowsupdate.com") ||
           EndsWith(h, ".delivery.mp.microsoft.com") || h == "download.microsoft.com";
}

inline std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

inline std::string BuildDownloadForm(const std::string& guid) {
    std::string json = "[{\"size\":0,\"languages\":\"\",\"uidInfo\":\"" + guid + "\",\"updateID\":\"" + guid + "\"}]";
    return "updateIDs=" + UrlEncode(json);
}

// Keep results that match this PC's architecture (titles say "x64-based", "ARM64-based" or "x86-based").
inline bool MatchesArch(const std::string& title, bool isArm64) {
    auto has = [&](const char* w) { return title.find(w) != std::string::npos; };
    if (isArm64) return !(has("x64-based") || has("x86-based"));
    return !(has("ARM64-based") || has("x86-based"));
}

// Safe file name from a URL (last path segment).
inline std::string FileNameFromUrl(const std::string& url) {
    size_t q = url.find_first_of("?#");
    std::string u = url.substr(0, q);
    size_t s = u.find_last_of('/');
    std::string n = s == std::string::npos ? u : u.substr(s + 1);
    std::string o;
    for (char c : n) if (std::isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_') o += c;
    return o.empty() ? "update.cab" : o;
}

inline bool IsPackageFile(const std::string& name) {
    std::string l;
    for (char c : name) l += (char)std::tolower((unsigned char)c);
    return EndsWith(l, ".msu") || EndsWith(l, ".cab");
}

} // namespace cat
