// MTNet -- the game's network layer, in-process, as a plugin of the loader.
//
// MTNet used to be a Python reverse proxy that had to be started before the
// game: the pak pointed the game at localhost:81/82 and the proxy forwarded
// the traffic on to the real servers, tempering the responses with templates.
// As a plugin it starts with the game, in the same process, and the port it
// listens on comes from the loader's ini -- the same merged ini every other
// plugin uses, so two plugins that both want a setting cannot overwrite each
// other (see the loader's ini ownership).
//
// The game is pointed at the proxy the same way the pak did it: the two
// mh.*ServerUrl console variables in the game's own UserEngine.ini. The plugin
// writes that file (the game's config folder is not the loader's ini folder,
// so this one file is written here rather than through the store).
//
// Settings: Binaries\Win64\mt_net.ini, [MTNet]
//   Port      the host server port the game is pointed at   (default 81)
//   WebPort   the web server port                            (default 82)
//   Target    the real host server        (default https://mt.racers.co.kr/)
//   WebTarget the real web server         (default https://www.racers.co.kr)
//   Log       1 to log every request                       (default 0)
// and [CVars] -- the console variables to hand the game, key=value lines.
//
// shortcut: the proxy forwards what it does not understand, so a game that
// asks for something new still works -- it just is not tempered yet.
// The panel is opened in the browser with CTRL+F9 (see openPanel).

#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../include/mt_plugin.hpp"
#include "../src/ini_merge.h"

using namespace mt;

namespace {

constexpr wchar_t kHostUrlVar[] = L"mh.hostServerUrl";
constexpr wchar_t kWebUrlVar[] = L"mh.webServerUrl";

struct Cfg {
    int port = 81, webPort = 82;
    std::wstring target = L"https://mt.racers.co.kr/", webTarget = L"https://www.racers.co.kr";
    bool log = false;
};

Cfg g_cfg;
std::atomic<bool> g_stop{false};
std::mutex g_logLock;

std::wstring toWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string toNarrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// The game's config folder: <game>\MotorTown\Config -- the same folder the
// MTNet pak put UserEngine.ini in, next to the game, and the folder the game
// reads its own config from at start-up.
std::wstring gameConfigDir() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* p = wcsrchr(exe, L'\\');   // ...MotorTown, Binaries, Win64
    if (!p) return L"";
    *p = 0;
    p = wcsrchr(exe, L'\\');             // ...MotorTown, Binaries
    if (!p) return L"";
    *p = 0;
    p = wcsrchr(exe, L'\\');             // ...MotorTown (the project folder)
    if (!p) return L"";
    *p = 0;
    return std::wstring(exe) + L"\\Config";
}

// Point the game at us: the same two console variables the pak set.
// The value has to be quoted. An ini treats // as a comment, so an unquoted
// http://localhost:82 is read as "http:" and the game never comes to us.
void writeGameUrls(int port, int webPort) {
    std::wstring dir = gameConfigDir();
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring path = dir + L"\\UserEngine.ini";
    char body[256];
    sprintf(body, "[ConsoleVariables]\r\nmh.hostServerUrl=\"http://localhost:%d/\"\r\nmh.webServerUrl=\"http://localhost:%d\"\r\n",
            port, webPort);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fputs(body, f);
    fclose(f);
    MT_LOG("game pointed at us: %s", body);
}

// ---- a small JSON subset: a flat object of scalars ----
// Enough for the config payloads ({"mh.x": 2.0}) without a dependency. The
// value is kept as its raw text, so a number stays a number when it is put
// back.
bool parseFlatJson(const std::string& body, std::vector<std::pair<std::string, std::string>>& out) {
    size_t i = body.find('{');
    if (i == std::string::npos) return false;
    for (i++; i < body.size();) {
        while (i < body.size() && (body[i] == ',' || isspace((unsigned char)body[i]))) i++;
        if (i >= body.size() || body[i] == '}') break;
        if (body[i] != '"') return false;
        size_t e = body.find('"', ++i);
        if (e == std::string::npos) return false;
        std::string key = body.substr(i, e - i);
        i = e + 1;
        while (i < body.size() && isspace((unsigned char)body[i])) i++;
        if (i >= body.size() || body[i] != ':') return false;
        i++;
        while (i < body.size() && isspace((unsigned char)body[i])) i++;
        std::string val;
        if (i < body.size() && body[i] == '"') {
            size_t ve = body.find('"', ++i);
            if (ve == std::string::npos) return false;
            val = body.substr(i, ve - i);   // the text, without the quotes
            i = ve + 1;
        } else {
            size_t ve = i;
            while (ve < body.size() && body[ve] != ',' && body[ve] != '}') ve++;
            val = trim(body.substr(i, ve - i));
            i = ve;
        }
        out.emplace_back(key, val);
    }
    return true;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:   o += c;      break;
        }
    }
    return o;
}

// A number, bool or null stays raw. Anything else is a string.
std::string jsonValue(const std::string& v) {
    if (v == "true" || v == "false" || v == "null") return v;
    if (!v.empty()) {
        char* end = nullptr;
        strtod(v.c_str(), &end);
        if (end && *end == 0) return v;
    }
    return "\"" + jsonEscape(v) + "\"";
}

std::string flatJson(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out = "{";
    bool first = true;
    for (auto& p : kv) {
        if (!first) out += ",";
        first = false;
        out += "\"" + jsonEscape(p.first) + "\":" + jsonValue(p.second);
    }
    out += "}";
    return out;
}

// /config, /config/, /config?x=1 -- not /configure.
bool isConfigPath(std::wstring path) {
    size_t q = path.find(L'?');
    if (q != std::wstring::npos) path.resize(q);
    while (!path.empty() && path.back() == L'/') path.pop_back();
    return path.size() >= 7 && _wcsicmp(path.c_str() + path.size() - 7, L"/config") == 0;
}

// The object after "configs", or the body itself if it is already flat.
std::string configsObject(const std::string& body) {
    size_t at = body.find("\"configs\"");
    if (at == std::string::npos) return body;
    size_t open = body.find('{', at);
    if (open == std::string::npos) return "";
    size_t close = body.find('}', open);
    if (close == std::string::npos) return "";
    return body.substr(open, close - open + 1);
}

std::string readFile(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return {};
    std::string s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

std::mutex g_cfgLock;
std::vector<std::pair<std::string, std::string>> g_serverConfigs;
std::string g_panel;               // the tablet page, served from disk

// What the server pushes at the game: the review questions ("How do you feel
// about the income generated by Company Taxis?"), the leaderboard, and the
// rest. It arrives as JSON and the game draws it in its own widgets, so there
// is no HTML to inject into -- the panel shows it instead.
struct Captured { std::string path, body; };
std::mutex g_capLock;
std::vector<Captured> g_captured;

bool looksInteresting(const std::wstring& path, const std::string& body) {
    std::string lp = lower(toNarrow(path)), lb = lower(body);
    if (lp.rfind("/event", 0) == 0) return true;
    static const char* keys[] = {"leaderboard", "leader_board", "feedback", "review", "question"};
    for (const char* k : keys)
        if (lb.find(k) != std::string::npos) return true;
    return false;
}

void capture(const std::wstring& path, const std::string& body) {
    if (body.empty() || body.size() > 200000) return;
    std::string p = toNarrow(path);
    std::lock_guard<std::mutex> g(g_capLock);
    for (auto& c : g_captured)
        if (c.path == p) { c.body = body; return; }
    g_captured.push_back({p, body});
    while (g_captured.size() > 8) g_captured.erase(g_captured.begin());
}

void rememberConfigs(const std::string& body) {
    std::vector<std::pair<std::string, std::string>> vars;
    if (!parseFlatJson(configsObject(body), vars) || vars.empty()) return;
    std::lock_guard<std::mutex> g(g_cfgLock);
    g_serverConfigs = std::move(vars);
}

std::vector<std::pair<std::string, std::string>> cvarOverrides();

std::vector<std::pair<std::string, std::string>> mergedConfigs() {
    std::vector<std::pair<std::string, std::string>> base;
    {
        std::lock_guard<std::mutex> g(g_cfgLock);
        base = g_serverConfigs;
    }
    for (auto& o : cvarOverrides()) {
        bool replaced = false;
        for (auto& s : base)
            if (s.first == o.first) { s.second = o.second; replaced = true; break; }
        if (!replaced) base.push_back(o);
    }
    return base;
}

std::wstring cfgIni() { return gameDir() + L"mt_net.ini"; }

// The console variables we want the game to run with, from [CVars].
std::vector<std::pair<std::string, std::string>> cvarOverrides() {
    std::vector<std::pair<std::string, std::string>> out;
    for (std::wstring& line : ini::sectionLines(cfgIni(), L"CVars")) {
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        out.emplace_back(toNarrow(line.substr(0, eq)), toNarrow(line.substr(eq + 1)));
    }
    return out;
}

// Put our console variables into the config the server sent, and hand it back.
// The game reads this and applies them -- that is how MTNet has always worked:
// the numbers the game runs with come from the config, not from a local file.
std::string temperConfig(const std::string& body, const std::vector<std::pair<std::string, std::string>>& ours) {
    size_t at = body.find("\"configs\"");
    if (at == std::string::npos) return body;
    size_t open = body.find('{', at);
    if (open == std::string::npos) return body;
    size_t close = body.find('}', open);
    if (close == std::string::npos) return body;

    std::vector<std::pair<std::string, std::string>> server;
    if (!parseFlatJson(body.substr(open, close - open + 1), server)) return body;

    // Ours win over the server's, key by key.
    for (auto& o : ours) {
        bool replaced = false;
        for (auto& s : server) {
            if (s.first == o.first) { s.second = o.second; replaced = true; break; }
        }
        if (!replaced) server.push_back(o);
    }

    std::string rebuilt = "{";
    bool first = true;
    for (auto& p : server) {
        if (!first) rebuilt += ",";
        first = false;
        rebuilt += "\"" + jsonEscape(p.first) + "\":" + jsonValue(p.second);
    }
    rebuilt += "}";
    return body.substr(0, open) + rebuilt + body.substr(close + 1);
}

// The gear icon is for the client. A dedicated server has no page to inject
// into; it opens a panel on its own port, and that panel is the only place a
// password exists. The client proxy stays open -- it only listens on localhost.
bool serverExe(const wchar_t* path) { return path && wcsstr(path, L"MotorTownServer") != nullptr; }

bool dedicatedServer() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return serverExe(exe);
}

bool sameSecret(const std::string& a, const std::string& b) {
    size_t n = a.size() > b.size() ? a.size() : b.size();
    unsigned diff = (unsigned)a.size() ^ (unsigned)b.size();
    for (size_t i = 0; i < n; i++) {
        unsigned ca = i < a.size() ? (unsigned char)a[i] : 0;
        unsigned cb = i < b.size() ? (unsigned char)b[i] : 0;
        diff |= ca ^ cb;
    }
    return diff == 0;
}

std::string randomText(int n) {
    static const char* alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    const int m = 55;
    std::string out(n, 'x');
    std::vector<unsigned char> buf(n);
    if (BCryptGenRandom(nullptr, buf.data(), (ULONG)buf.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return "";
    for (int i = 0; i < n; i++) out[i] = alphabet[buf[i] % m];
    return out;
}
bool selfCheck() {
    std::vector<std::pair<std::string, std::string>> ours{{"mh.x", "9"}};
    std::string got = temperConfig("{\"configs\":{\"mh.x\":1,\"mh.y\":2}}", ours);
    bool ok = got.find("\"mh.x\":9") != std::string::npos &&
              got.find("\"mh.y\":2") != std::string::npos &&
              got.find("\"mh.x\":1") == std::string::npos;
    std::vector<std::pair<std::string, std::string>> parsed;
    ok = ok && parseFlatJson("{\"a\":1,\"b\":\"x\"}", parsed) && parsed.size() == 2 &&
         parsed[0].first == "a" && parsed[0].second == "1" && parsed[1].second == "x";
    ok = ok && jsonValue("9") == "9" && jsonValue("hi") == "\"hi\"";
    ok = ok && isConfigPath(L"/config") && isConfigPath(L"/config?x=1") &&
         isConfigPath(L"/v1/config/") && !isConfigPath(L"/configure");
    ok = ok && sameSecret("abc", "abc") && !sameSecret("abc", "abd") && !sameSecret("ab", "abc");
    ok = ok && serverExe(L"C:\\srv\\MotorTownServer-Win64-Shipping.exe") &&
         !serverExe(L"C:\\game\\MotorTown-Win64-Shipping.exe");
    std::string rnd = randomText(20);
    ok = ok && rnd.size() == 20;
    MT_LOG("self-check: %s", ok ? "ok" : "FAILED");
    return ok;
}

// Write the console variables the game is running with to a file, so they can
// be read (and changed) outside the game.
void exportCvars(const std::string& body) {
    size_t at = body.find("\"configs\"");
    if (at == std::string::npos) return;
    size_t open = body.find('{', at);
    if (open == std::string::npos) return;
    size_t close = body.find('}', open);
    if (close == std::string::npos) return;
    std::vector<std::pair<std::string, std::string>> vars;
    if (!parseFlatJson(body.substr(open, close - open + 1), vars)) return;
    std::wstring path = gameDir() + L"mt_net_cvars.txt";
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) return;
    fprintf(f, "; the console variables the game is running with, written by MTNet\n");
    for (auto& v : vars) fprintf(f, "%s=%s\n", v.first.c_str(), v.second.c_str());
    fclose(f);
    MT_LOG("%zu console variable(s) exported to mt_net_cvars.txt", vars.size());
}

// What the gear panel asks for: {"configs": { ...server, ours win... }}.
std::string cvarJson() {
    return "{\"configs\":" + flatJson(mergedConfigs()) + "}";
}

// Save a config payload into [CVars]. The panel sends {"configs": {...}}.
// The section is rewritten, so a key the panel removed does not linger.
bool saveCvars(const std::string& body) {
    std::vector<std::pair<std::string, std::string>> kv;
    if (!parseFlatJson(configsObject(body), kv)) return false;
    std::wstring ini = cfgIni();
    WritePrivateProfileStringW(L"CVars", nullptr, nullptr, ini.c_str());
    for (auto& p : kv)
        WritePrivateProfileStringW(L"CVars", toWide(p.first).c_str(), toWide(p.second).c_str(), ini.c_str());
    MT_LOG("%zu console variable(s) saved", kv.size());
    return true;
}

// ---- HTTP, the small part of it ----
std::string panelPage();   // the panel page, read fresh on every request

struct Request {
    std::string method, target, version, headers, body;
};

std::string headerValue(const std::string& headers, const char* name) {
    std::string want = lower(name);
    size_t i = 0;
    while (i < headers.size()) {
        size_t eol = headers.find("\r\n", i);
        if (eol == std::string::npos) eol = headers.size();
        std::string line = headers.substr(i, eol - i);
        size_t colon = line.find(':');
        if (colon != std::string::npos && lower(line.substr(0, colon)) == want)
            return trim(line.substr(colon + 1));
        i = eol + 2;
    }
    return "";
}

// Read one request off a socket. Returns false if the peer hung up.
bool readRequest(SOCKET s, Request& req) {
    std::string buf;
    char tmp[8192];
    size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        headEnd = buf.find("\r\n\r\n");
        if (buf.size() > 1 << 20) return false;
    }
    size_t lineEnd = buf.find("\r\n");
    std::string line = buf.substr(0, lineEnd);
    size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;
    req.method = line.substr(0, sp1);
    req.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = line.substr(sp2 + 1);
    size_t bodyStart = headEnd + 4;
    req.headers = buf.substr(lineEnd + 2, headEnd - lineEnd - 2);

    size_t want = 0;
    std::string len = headerValue(req.headers, "content-length");
    if (!len.empty()) want = (size_t)atoll(len.c_str());
    while (buf.size() - bodyStart < want) {
        int n = recv(s, tmp, sizeof tmp, 0);
        if (n <= 0) break;
        buf.append(tmp, n);
    }
    size_t have = buf.size() - bodyStart;
    req.body = buf.substr(bodyStart, want && have > want ? want : have);
    return true;
}

bool sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

// The client's own headers, minus the ones that belong to the hop to us.
// The session cookie is in here, so it has to go on to the real server.
std::string upstreamHeaders(const std::string& headers) {
    std::string out;
    size_t i = 0;
    while (i < headers.size()) {
        size_t eol = headers.find("\r\n", i);
        if (eol == std::string::npos) eol = headers.size();
        std::string line = headers.substr(i, eol - i);
        i = eol + 2;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = lower(trim(line.substr(0, colon)));
        if (name == "host" || name == "connection" || name == "content-length" ||
            name == "accept-encoding" || name == "proxy-connection")
            continue;
        out += line + "\r\n";
    }
    return out;
}

// Forward one request upstream and relay the answer.
// inject: the web port. HTML answers get the gear icon inserted.
std::string forward(const std::wstring& target, const Request& req, bool inject) {
    std::wstring t = target;
    bool secure = t.rfind(L"https://", 0) == 0;
    if (secure) t = t.substr(8);
    else if (t.rfind(L"http://", 0) == 0) t = t.substr(7);
    size_t slash = t.find(L'/');
    std::wstring host = slash == std::wstring::npos ? t : t.substr(0, slash);
    std::wstring path = slash == std::wstring::npos ? L"/" : t.substr(slash);
    if (!req.target.empty() && req.target[0] == '/') path = toWide(req.target);
    int port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = host.find(L':');
    if (colon != std::wstring::npos) { port = _wtoi(host.c_str() + colon + 1); host = host.substr(0, colon); }

    const char* bad = "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n";
    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) MTNet/1.0",
                                    WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return bad;
    // The game is waiting on our answer, so do not wait long for the real one.
    DWORD connect = 10000, receive = 15000;
    WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_TIMEOUT, &connect, sizeof connect);
    WinHttpSetOption(session, WINHTTP_OPTION_RECEIVE_TIMEOUT, &receive, sizeof receive);
    HINTERNET conn = WinHttpConnect(session, host.c_str(), (INTERNET_PORT)port, 0);
    HINTERNET reqH = conn ? WinHttpOpenRequest(conn, req.method == "GET" ? L"GET" : L"POST", path.c_str(),
                                               nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               secure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    std::string answer;
    if (reqH) {
        std::wstring type = toWide(headerValue(req.headers, "content-type"));
        if (type.empty()) type = L"application/json";
        // WinHTTP wants whole header lines -- a bare content type is an
        // invalid parameter, and every request would come back 502.
        std::wstring headers = L"Content-Type: " + type + L"\r\n";
        std::wstring extra = toWide(upstreamHeaders(req.headers));
        if (!extra.empty()) headers += extra;
        headers += L"\r\n";
        if (WinHttpSendRequest(reqH, headers.c_str(), -1, (LPVOID)req.body.data(),
                                (DWORD)req.body.size(), (DWORD)req.body.size(), 0)) {
            WinHttpReceiveResponse(reqH, nullptr);
            DWORD status = 0, size = sizeof status;
            WinHttpQueryHeaders(reqH, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD len = 0;
            WinHttpQueryDataAvailable(reqH, &len);
            std::string body;
            while (len > 0) {
                DWORD got = 0;
                std::vector<char> chunk(len);
                if (!WinHttpReadData(reqH, chunk.data(), len, &got)) break;
                body.append(chunk.data(), got);
                len = 0;
                WinHttpQueryDataAvailable(reqH, &len);
            }
            if (looksInteresting(path, body)) capture(path, body);
            wchar_t ctypeW[256] = {0};
            DWORD ctLen = sizeof ctypeW;
            std::string contentType = "application/octet-stream";
            if (WinHttpQueryHeaders(reqH, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX,
                                    ctypeW, &ctLen, WINHTTP_NO_HEADER_INDEX) && ctypeW[0])
                contentType = toNarrow(ctypeW);
            std::string typeLow = lower(contentType);
            // The config is where the console variables live: ours go into it,
            // and what the game ends up running with is written out.
            if (isConfigPath(path) || (typeLow.find("json") != std::string::npos &&
                                       body.find("\"configs\"") != std::string::npos)) {
                rememberConfigs(body);
                body = temperConfig(body, cvarOverrides());
                exportCvars(body);
                MT_LOG("config: status %lu, %zu bytes", status, body.size());
            }
            // Give the game its cookies back.
            wchar_t cookie[2048] = {0};
            DWORD clen = sizeof cookie;
            std::string extraOut;
            if (WinHttpQueryHeaders(reqH, WINHTTP_QUERY_SET_COOKIE, WINHTTP_HEADER_NAME_BY_INDEX,
                                    cookie, &clen, WINHTTP_NO_HEADER_INDEX) && cookie[0])
                extraOut = "Set-Cookie: " + toNarrow(cookie) + "\r\n";
            answer = "HTTP/1.1 " + std::to_string(status) + " OK\r\n" + extraOut +
                     "Content-Type: " + contentType + "\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        } else {
            MT_LOG("upstream %ls: send failed (%lu)", host.c_str(), GetLastError());
        }
        WinHttpCloseHandle(reqH);
    } else {
        MT_LOG("upstream %ls: no request (%lu)", host.c_str(), GetLastError());
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return answer.empty() ? std::string(bad) : answer;
}

// A response with the headers a client needs, for pages we serve ourselves.
std::string httpRaw(const std::string& status, const std::string& type, const std::string& body, const std::string& extra = "");

// ---- the templates: the player's saved configurations ----
// They live in mt_net_templates\*.json next to the game exe, in MTNet's own
// format: {"title": "...", "configs": {...}}.
std::wstring templatesDir() { return gameDir() + L"mt_net_templates"; }

std::string jsonString(const std::string& body, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t at = body.find(pat);
    if (at == std::string::npos) return "";
    size_t colon = body.find(':', at + pat.size());
    if (colon == std::string::npos) return "";
    size_t q = body.find('"', colon);
    if (q == std::string::npos) return "";
    size_t e = body.find('"', q + 1);
    if (e == std::string::npos) return "";
    return body.substr(q + 1, e - q - 1);
}

struct Template {
    std::string name, title, description, author;
    std::vector<std::pair<std::string, std::string>> configs;
};

std::vector<Template> loadTemplates() {
    std::vector<Template> out;
    WIN32_FIND_DATAW fd;
    std::wstring pattern = templatesDir() + L"\\*.json";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring path = templatesDir() + L"\\" + fd.cFileName;
        std::string body = readFile(path);
        if (body.empty()) continue;
        Template t;
        std::wstring file = fd.cFileName;
        t.name = toNarrow(file.substr(0, file.find(L'.')));
        t.title = jsonString(body, "title");
        t.description = jsonString(body, "description");
        t.author = jsonString(body, "author");
        if (!parseFlatJson(configsObject(body), t.configs)) t.configs.clear();
        out.push_back(t);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// ---- the engine's console variables ----
// They are registered by FName, so their names are entries in the engine's
// name pool -- not strings in the binary (an earlier version scanned the exe
// and found 149 dotted strings that were not console variables at all).
// A console variable's name is a short lower-case prefix, a dot, and a name:
// r.ScreenPercentage, t.MaxFPS, fx.NiagaraAllowComputeShaders.
bool isCVarName(const std::string& s) {
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot > 6) return false;
    for (size_t i = 0; i < dot; i++) {
        char c = s[i];
        if (c < 'a' || c > 'z') return false;
    }
    for (size_t i = dot + 1; i < s.size(); i++) {
        char c = s[i];
        if (!isalnum((unsigned char)c) && c != '_') return false;
    }
    return true;
}

int collectCVar(const char* name, void* ctx) {
    auto* out = (std::vector<std::string>*)ctx;
    std::string s(name);
    if (isCVarName(s)) out->push_back(s);
    return 0;
}

std::vector<std::string> engineCVars() {
    // Not cached: the game registers console variables as their modules load,
    // so the catalogue grows while it runs (t.* and wp.* appear once the
    // world and the engine's own modules are up). Each request walks the
    // pool again, which is a few milliseconds on a background thread.
    std::vector<std::string> out;
    api->pool_names(collectCVar, &out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    MT_LOG("engine catalogue: %zu console variable name(s) out of the name pool", out.size());
    return out;
}

// A catalogue name is stricter than a live FName hit: the module scan sees a
// lot of dotted tokens, so a name must look like a real cvar -- a PascalCase
// word with at least two lower-case letters (drops hash-like junk: t.0E, a.a4a).
bool catalogueName(const std::string& s) {
    size_t dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot > 6) return false;
    if (s.find('.', dot + 1) != std::string::npos) return false;
    for (size_t i = 0; i < dot; i++) if (s[i] < 'a' || s[i] > 'z') return false;
    std::string n = s.substr(dot + 1);
    if (n.size() < 3 || !isalpha((unsigned char)n[0])) return false;
    int lower = 0;
    for (char c : n) {
        if (!isalnum((unsigned char)c) && c != '_') return false;
        if (c >= 'a' && c <= 'z') lower++;
    }
    return lower >= 2;
}

void harvestCVars(const std::string& run, std::vector<std::string>& out) {
    size_t i = 0, n = run.size();
    while (i < n) {
        char c = run[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) { i++; continue; }
        size_t j = i;
        while (j < n && (isalnum((unsigned char)run[j]) || run[j] == '_' || run[j] == '.')) j++;
        std::string t = run.substr(i, j - i);
        if (t.size() >= 3 && t.size() <= 48 && catalogueName(t)) out.push_back(t);
        i = j;
    }
}

// The engine's full console-variable catalogue (2500+ names, Motor Town's own
// mh.* among them) lives as string literals in the game module -- names the
// FName pool never holds. Scanned by shape from the data sections, no offsets.
// Cached once: compiled-in literals do not change after load.
std::vector<std::string> moduleCVars() {
    static std::vector<std::string> cache;
    static std::once_flag once;
    std::call_once(once, [] {
        auto base = (const uint8_t*)api->module_base();
        if (!base) return;
        auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        std::vector<std::string> hits;
        for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++) {
            // String literals live in the data sections; .text is code (noise)
            // and .pdata is unwind tables.
            if (memcmp(sec->Name, ".text", 5) == 0) continue;
            if (memcmp(sec->Name, ".pdata", 6) == 0) continue;
            const uint8_t* lo = base + sec->VirtualAddress;
            size_t n = sec->Misc.VirtualSize;
            std::string run;
            for (size_t i = 0; i < n; i++) {               // 8-bit strings
                uint8_t b = lo[i];
                if (b >= 0x20 && b < 0x7f) run.push_back((char)b);
                else { if (run.size() >= 3) harvestCVars(run, hits); run.clear(); }
            }
            if (run.size() >= 3) harvestCVars(run, hits);
            run.clear();
            for (size_t i = 0; i + 1 < n; i += 2) {        // UTF-16LE strings
                if (lo[i + 1] == 0 && lo[i] >= 0x20 && lo[i] < 0x7f) run.push_back((char)lo[i]);
                else { if (run.size() >= 3) harvestCVars(run, hits); run.clear(); }
            }
            if (run.size() >= 3) harvestCVars(run, hits);
        }
        std::sort(hits.begin(), hits.end());
        hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
        cache.swap(hits);
        MT_LOG("engine catalogue: %zu console variable name(s) scanned from the game module", cache.size());
    });
    return cache;
}

// ---- the API the pak's UI talks to ----
std::string apiAnswer(const Request& req) {
    std::string path = req.target;
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    if (path == "/api/get-config") return cvarJson();
    if (path == "/api/save-config")
        return saveCvars(req.body)
            ? "{\"status\":\"saved\",\"message\":\"Configuration saved! Restart the game for the numbers to apply.\"}"
            : "{\"status\":\"error\",\"message\":\"could not read that config\"}";
    if (path == "/api/reset-config") {
        WritePrivateProfileStringW(L"CVars", nullptr, nullptr, cfgIni().c_str());
        return "{\"status\":\"reset\",\"message\":\"Configuration reset.\"}";
    }
    if (path == "/api/all-vars") {
        std::vector<std::string> names = moduleCVars();     // the compiled-in catalogue
        std::vector<std::string> live = engineCVars();      // plus whatever is live now
        names.insert(names.end(), live.begin(), live.end());
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        std::string out = "[";
        for (size_t i = 0; i < names.size(); i++) {
            if (i) out += ",";
            out += "\"" + jsonEscape(names[i]) + "\"";
        }
        out += "]";
        return out;
    }
    if (path == "/api/templates") {
        std::string out = "[";
        std::vector<Template> ts = loadTemplates();
        for (size_t i = 0; i < ts.size(); i++) {
            if (i) out += ",";
            out += "{\"name\":\"" + jsonEscape(ts[i].name) + "\",\"title\":\"" + jsonEscape(ts[i].title) +
                   "\",\"description\":\"" + jsonEscape(ts[i].description) + "\",\"author\":\"" + jsonEscape(ts[i].author) +
                   "\",\"variables\":" + std::to_string(ts[i].configs.size()) +
                   ",\"configs\":" + flatJson(ts[i].configs) + "}";
        }
        out += "]";
        return out;
    }
    if (path == "/api/apply-template") {
        std::string name = jsonString(req.body, "name");
        std::vector<Template> ts = loadTemplates();
        for (auto& t : ts) {
            if (t.name != name) continue;
            WritePrivateProfileStringW(L"CVars", nullptr, nullptr, cfgIni().c_str());
            for (auto& p : t.configs)
                WritePrivateProfileStringW(L"CVars", toWide(p.first).c_str(), toWide(p.second).c_str(), cfgIni().c_str());
            MT_LOG("template %s applied: %zu variable(s)", t.name.c_str(), t.configs.size());
            return "{\"status\":\"applied\",\"message\":\"" +
                   jsonEscape(t.title.empty() ? t.name : t.title) + " applied. It takes effect on the next game start.\"}";
        }
        return "{\"status\":\"error\",\"message\":\"no template called that\"}";
    }
    if (path == "/api/last-events") {
        std::lock_guard<std::mutex> g(g_capLock);
        std::string out = "[";
        for (size_t i = 0; i < g_captured.size(); i++) {
            if (i) out += ",";
            out += "{\"path\":\"" + jsonEscape(g_captured[i].path) + "\",\"body\":\"" +
                   jsonEscape(g_captured[i].body) + "\"}";
        }
        out += "]";
        return out;
    }
    if (path == "/api/list-templates") return "{\"templates\":[]}";
    if (path == "/api/save-template" || path == "/api/load-template")
        return "{\"status\":\"error\",\"message\":\"templates are not in this build yet\"}";
    return "";
}

// A response, with the headers a client needs. One request per connection:
// the game is not a browser and does not want keep-alive.
// A response, with the headers a client needs. One request per connection:
// the game is not a browser and does not want keep-alive. Nothing is cached:
// the panel is a page that changes under the player's hands.
std::string httpAnswer(const std::string& body, const char* status = "200 OK") {
    return "HTTP/1.1 " + std::string(status) + "\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
}

void serve(SOCKET client, const std::wstring& target, bool inject) {
    Request req;
    if (!readRequest(client, req)) {
        MT_LOG("accept: a client connected and left without a request");
        closesocket(client);
        return;
    }
    if (g_cfg.log || inject) {
        static std::atomic<int> seen{0};
        if (g_cfg.log || seen.fetch_add(1) < 12) {
            std::lock_guard<std::mutex> g(g_logLock);
            MT_LOG("%s %s", req.method.c_str(), req.target.c_str());
        }
    }
    std::string answer = apiAnswer(req);
    if (!answer.empty()) {
        answer = httpAnswer(answer);                    // our own API
        MT_LOG("api %s -> %zu bytes", req.target.c_str(), answer.size());
    } else if (req.target == "/" || req.target == "/panel") {
        std::string page = panelPage();
        answer = page.empty() ? httpAnswer("{\"error\":\"panel missing\"}", "404 Not Found")
                              : httpRaw("200 OK", "text/html; charset=utf-8", page);
    } else if (req.target.rfind("/api/", 0) == 0) {
        answer = httpAnswer("{\"error\":\"not found\"}", "404 Not Found");
    } else {
        answer = forward(target, req, inject);          // already a whole response
    }
    sendAll(client, answer);
    shutdown(client, SD_SEND);
    closesocket(client);
}

void listener(int port, const std::wstring& target, bool inject) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { MT_LOG("socket failed"); return; }
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // the game, on this machine
    addr.sin_port = htons((u_short)port);
    if (bind(s, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR) {
        MT_LOG("port %d is taken -- not listening on it", port);
        closesocket(s);
        return;
    }
    if (listen(s, 16) == SOCKET_ERROR) { closesocket(s); return; }
    MT_LOG("listening on 127.0.0.1:%d -> %ls", port, target.c_str());
    while (!g_stop) {
        SOCKET client = accept(s, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread(serve, client, target, inject).detach();
    }
    closesocket(s);
}

std::string g_password, g_session;

std::string urlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string postedPassword(const Request& req) {
    std::string h = headerValue(req.headers, "x-mtnet-password");
    if (!h.empty()) return h;
    size_t j = req.body.find("\"password\"");
    if (j != std::string::npos) {
        size_t q = req.body.find('"', req.body.find(':', j) + 1);
        if (q != std::string::npos) {
            size_t e = req.body.find('"', q + 1);
            if (e != std::string::npos) return req.body.substr(q + 1, e - q - 1);
        }
    }
    size_t i = req.body.find("password=");
    if (i == std::string::npos) return "";
    i += 9;
    size_t e = req.body.find('&', i);
    return urlDecode(req.body.substr(i, e == std::string::npos ? std::string::npos : e - i));
}

std::string cookieToken(const std::string& headers) {
    std::string c = headerValue(headers, "cookie");
    size_t i = c.find("mtnet=");
    if (i == std::string::npos) return "";
    i += 6;
    size_t e = c.find(';', i);
    return trim(c.substr(i, e == std::string::npos ? std::string::npos : e - i));
}

bool panelAuthed(const Request& req) {
    if (sameSecret(cookieToken(req.headers), g_session) && !g_session.empty()) return true;
    std::string h = headerValue(req.headers, "x-mtnet-password");
    return !h.empty() && sameSecret(h, g_password);
}

std::string httpRaw(const std::string& status, const std::string& type, const std::string& body, const std::string& extra) {
    return "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\n" + extra +
           "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

std::string loginPage(bool wrong) {
    std::string body =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>MTNet</title></head>"
        "<body style=\"font-family:sans-serif;background:#111;color:#eee;padding:40px\">"
        "<h3>MTNet server</h3>"
        "<form method=\"POST\" action=\"/login\"><input name=\"password\" type=\"password\" autofocus>"
        "<button>Log in</button></form>";
    if (wrong) body += "<p style=\"color:#f66\">wrong password</p>";
    body += "</body></html>";
    return httpRaw(wrong ? "401 Unauthorized" : "200 OK", "text/html; charset=utf-8", body);
}

std::string panelPage() {
    // Fresh from disk every request: edit mt_net_panel.html, reload, no restart.
    // Returns the body only (empty when missing); callers frame it / 404.
    return readFile(gameDir() + L"mt_net_panel.html");
}

void servePanel(SOCKET client) {
    Request req;
    if (!readRequest(client, req)) { closesocket(client); return; }
    std::string path = req.target;
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    std::string answer;
    if (path == "/login" && req.method == "POST") {
        if (sameSecret(postedPassword(req), g_password)) {
            answer = "HTTP/1.1 302 Found\r\nSet-Cookie: mtnet=" + g_session +
                     "; HttpOnly; Path=/\r\nLocation: /\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            answer = loginPage(true);
        }
    } else if (!panelAuthed(req)) {
        answer = path.rfind("/api/", 0) == 0 ? httpAnswer("{\"error\":\"login\"}", "401 Unauthorized") : loginPage(false);
    } else if (path == "/" || path.empty()) {
        std::string page = panelPage();
        answer = page.empty() ? httpAnswer("{\"error\":\"panel missing\"}", "404 Not Found")
                              : httpRaw("200 OK", "text/html; charset=utf-8", page);
    } else {
        std::string api = apiAnswer(req);
        answer = api.empty() ? httpAnswer("{\"error\":\"not found\"}", "404 Not Found") : httpAnswer(api);
    }
    sendAll(client, answer);
    shutdown(client, SD_SEND);
    closesocket(client);
}

void panelListener(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return;
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((u_short)port);
    if (bind(s, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR) {
        MT_LOG("panel port %d is taken", port);
        closesocket(s);
        return;
    }
    if (listen(s, 8) == SOCKET_ERROR) { closesocket(s); return; }
    MT_LOG("server panel on 0.0.0.0:%d -- password is in mt_net.ini [MTNet] Password", port);
    MT_LOG("panel password: %s", g_password.c_str());
    // shortcut: the panel is HTTP. A password on cleartext is not safe on the
    // open internet -- do not publish this port until the panel speaks TLS.
    while (!g_stop) {
        SOCKET client = accept(s, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::thread(servePanel, client).detach();
    }
    closesocket(s);
}

// ---- the tablet: CTRL+F9 puts our panel in the game's own browser ----
// The game draws its UI in UMG, but it also has a UWebBrowser (the in-game
// web view). We do not draw anything ourselves: we tell that browser to load
// our panel, and the panel is our own page. That is the whole trick -- the
// game draws it, we serve it.
std::string g_panelUnused;         // (kept: the panel page lives above)
std::string g_gamePage;          // the page the game was showing, so we can go back
std::atomic<bool> g_tabletOpen{false};

std::wstring g_panelUrl() {
    return L"http://localhost:" + std::to_wstring(g_cfg.webPort) + L"/panel";
}

// The properties of a function, with their offsets inside its parameter
// block: the block is a struct, and we fill it by hand.
struct Param { std::string name; int offset; };

std::vector<Param> fnParams(uintptr_t func) {
    std::vector<Param> out;
    uintptr_t p = get<uintptr_t>(func + L().struct_child_props);
    while (p) {
        out.push_back({name(get<uint32_t>(p + L().field_name)), get<int32_t>(p + L().field_offset)});
        p = get<uintptr_t>(p + L().field_next);
    }
    return out;
}

// An FString property: a TArray of wchar, {data, count, max}.
std::string readStringProp(uintptr_t func, void* parms, const char* prop) {
    for (auto& p : fnParams(func)) {
        if (p.name != prop) continue;
        const wchar_t* data = *(const wchar_t**)((char*)parms + p.offset);
        int count = *(int32_t*)((char*)parms + p.offset + sizeof(const wchar_t*));
        if (!data || count <= 0 || count > 4096) return {};
        return toNarrow(std::wstring(data, (size_t)count));
    }
    return {};
}

int onLoadURL(uintptr_t, uintptr_t func, void* parms, void*) {
    std::string url = readStringProp(func, parms, "NewURL");
    // Remember the game's own page, so the second press can put it back.
    if (!url.empty() && url.find("localhost") == std::string::npos) g_gamePage = url;
    return 0;
}

// ---- the tablet: a UMG widget the game draws, holding our panel page ----
// The game's UI is UMG, and it already ships widgets that contain a
// UWebBrowser (the leaderboard, the main menu). We do not draw anything and
// we do not create objects: UWidgetBlueprintLibrary::Create builds the
// widget for us, AddToViewport puts it on screen, and we point its browser at
// our panel. Everything stays in this dll -- no extra dependency, no change
// to the loader.
uintptr_t g_tablet = 0;          // the widget instance we created
uintptr_t g_tabletBrowser = 0;   // its web browser

// Class lookups walk the whole object table, and that table is huge in a
// running game. So: compare name indices instead of strings, and remember
// what was found. A name that is not loaded yet is not remembered -- it may
// arrive later.
struct ClassCache { const char* name; uintptr_t cls; };
ClassCache g_classCache[24];
int g_classCacheCount = 0;

uintptr_t findClassByName(const char* want) {
    for (int i = 0; i < g_classCacheCount; i++)
        if (!strcmp(g_classCache[i].name, want)) return g_classCache[i].cls;
    uint32_t idx = (uint32_t)api->name_index(want);
    if (!idx) return 0;
    uintptr_t found = 0;
    int n = api->num_objects();
    for (int i = 0; i < n && !found; i++) {
        uintptr_t o = api->object(i);
        if (!o || get<uint32_t>(o + L().obj_name) != idx) continue;
        if (objName(classOf(o)) != "Class") continue;
        found = o;
    }
    if (found && g_classCacheCount < 24) {
        g_classCache[g_classCacheCount].name = want;
        g_classCache[g_classCacheCount].cls = found;
        g_classCacheCount++;
    }
    return found;
}

uintptr_t findCDO(uintptr_t cls) {
    int n = api->num_objects();
    for (int i = 0; i < n; i++) {
        uintptr_t o = api->object(i);
        if (o && classOf(o) == cls) return o;
    }
    return 0;
}

uintptr_t findPlayerController() {
    uintptr_t cls = findClassByName("PlayerController");
    if (!cls) return 0;
    int n = api->num_objects();
    uintptr_t fallback = 0;
    for (int i = 0; i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o || !api->is_a(classOf(o), cls)) continue;
        // The class default object is in the table too and it has no world:
        // hand it to the engine as a world context and every widget call
        // comes back empty.
        if (isCDO(o)) continue;
        if (!fallback) fallback = o;
        // A local player's controller is the one that has a Player.
        int off = api->field_offset(cls, "Player");
        if (off >= 0 && get<uintptr_t>(o + off)) return o;
    }
    return fallback;
}

// A static function needs the class default object as its "this".
uintptr_t functionOwner(const char* fnName) {
    uintptr_t fn = api->find_object("Function", fnName, "WidgetBlueprintLibrary");
    if (!fn) return 0;
    uintptr_t cls = findClassByName("WidgetBlueprintLibrary");
    return cls ? findCDO(cls) : 0;
}

void callRemoveFromParent(uintptr_t widget) {
    uintptr_t fn = api->find_object("Function", "RemoveFromParent", "Widget");
    if (!fn) { MT_LOG("tablet: RemoveFromParent is not in the game"); return; }
    char parms[64] = {0};
    api->process_event(widget, fn, parms);
}

void callAddToViewport(uintptr_t widget, int z) {
    uintptr_t fn = api->find_object("Function", "AddToViewport", "UserWidget");
    if (!fn) { MT_LOG("tablet: AddToViewport is not in the game"); return; }
    auto ps = fnParams(fn);
    int off = 0;
    for (auto& p : ps)
        if (p.name == "ZOrder") off = p.offset;
    char parms[64] = {0};
    int32_t z32 = z;
    memcpy(parms + off, &z32, sizeof z32);
    api->process_event(widget, fn, parms);
}

// UWidgetBlueprintLibrary::Create(WorldContextObject, WidgetType, OwningPlayer)
uintptr_t createWidget(uintptr_t cls, uintptr_t pc) {
    uintptr_t fn = api->find_object("Function", "Create", "WidgetBlueprintLibrary");
    uintptr_t owner = functionOwner("Create");
    if (!fn || !owner) {
        MT_LOG("tablet: Create is missing (fn %p, owner %p)", (void*)fn, (void*)owner);
        return 0;
    }
    uintptr_t userWidget = findClassByName("UserWidget");
    MT_LOG("tablet: Create(%s): a UserWidget? %s", objName(cls).c_str(),
           (userWidget && api->is_a(cls, userWidget)) ? "yes" : "NO");
    auto ps = fnParams(fn);
    for (auto& p : ps) MT_LOG("tablet: Create param %s at %d", p.name.c_str(), p.offset);
    uintptr_t vals[3] = {pc, cls, pc};
    const char* pnames[3] = {"WorldContextObject", "WidgetType", "OwningPlayer"};
    char parms[128] = {0};
    int retOff = -1;
    for (int i = 0; i < 3; i++) {
        int off = -1;
        for (auto& p : ps) {
            if (p.name == pnames[i]) off = p.offset;
            if (p.name == "ReturnValue") retOff = p.offset;
        }
        if (off < 0) { MT_LOG("tablet: Create has no %s", pnames[i]); return 0; }
        memcpy(parms + off, &vals[i], sizeof(uintptr_t));
    }
    if (retOff < 0) { MT_LOG("tablet: Create has no return value"); return 0; }
    api->process_event(owner, fn, parms);
    uintptr_t widget = 0;
    memcpy(&widget, parms + retOff, sizeof widget);
    MT_LOG("tablet: Create(%s) -> %p", objName(cls).c_str(), (void*)widget);
    return widget;
}

// A web browser the game already has on screen, if it has one.
uintptr_t findLiveBrowser() {
    uintptr_t cls = findClassByName("WebBrowser");
    if (!cls) return 0;
    int n = api->num_objects();
    for (int i = 0; i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o || classOf(o) != cls) continue;
        // The class default object is a browser too, and it is first in the
        // table: pointing it at a URL goes nowhere.
        if (isCDO(o)) continue;
        return o;
    }
    return 0;
}

// The widget's own web browser, whichever of the game's widgets we used.
uintptr_t widgetBrowser(uintptr_t widget, uintptr_t cls) {
    static const char* props[] = {"MotorTownWeb", "NoticeWebBrowser", "WebBrowser"};
    for (const char* prop : props) {
        int off = api->field_offset(cls, prop);
        if (off < 0) { MT_LOG("tablet: %s has no property %s", objName(cls).c_str(), prop); continue; }
        uintptr_t browser = get<uintptr_t>(widget + off);
        MT_LOG("tablet: %s.%s -> %p", objName(cls).c_str(), prop, (void*)browser);
        if (browser) return browser;
    }
    return 0;
}

void callLoadURL(uintptr_t browser, const std::wstring& url) {
    uintptr_t fn = api->find_object("Function", "LoadURL", "WebBrowser");
    if (!fn) { MT_LOG("tablet: LoadURL is not in the game"); return; }
    int off = 0;
    for (auto& p : fnParams(fn))
        if (p.name == "NewURL") off = p.offset;
    struct { const wchar_t* data; int32_t count; int32_t max; } fs{url.c_str(), (int32_t)url.size(), (int32_t)url.size() + 1};
    char parms[256] = {0};
    memcpy(parms + off, &fs, sizeof fs);
    api->process_event(browser, fn, parms);
}

// Any widget class of the game that holds a web browser, found by shape
// rather than by name: the property's own class is a UWebBrowser.
uintptr_t findWebWidgetClass(std::string& propName) {
    uintptr_t userWidget = findClassByName("UserWidget");
    uintptr_t webBrowser = findClassByName("WebBrowser");
    if (!userWidget || !webBrowser) return 0;
    int n = api->num_objects();
    for (int i = 0; i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o || objName(classOf(o)) != "Class") continue;   // a class
        if (!api->is_a(o, userWidget)) continue;                // a UUserWidget
        for (uintptr_t c = o; c; c = get<uintptr_t>(c + L().struct_super)) {
            for (uintptr_t f = get<uintptr_t>(c + L().struct_child_props); f; f = get<uintptr_t>(f + L().field_next)) {
                if (get<uintptr_t>(f + 8) != webBrowser) continue;   // the FField's class
                propName = name(get<uint32_t>(f + L().field_name));
                return o;
            }
        }
    }
    return 0;
}

// The blueprint of a native widget class: it is the one with a widget tree,
// and the one whose web browser is filled in. It is in the object table once
// the player has seen that screen.
uintptr_t findSubclass(uintptr_t base) {
    static uintptr_t cached = 0;
    if (cached) return cached;
    uintptr_t classClass = findClassByName("Class");
    if (!classClass) return 0;
    int n = api->num_objects();
    for (int i = 0; i < n; i++) {
        uintptr_t o = api->object(i);
        if (!o || o == base) continue;
        if (classOf(o) != classClass) continue;      // only classes
        if (api->is_a(o, base)) { cached = o; return o; }
    }
    return 0;
}

// The widget that owns a browser: a browser lives in a widget tree, and the
// tree lives in the widget.
uintptr_t ownerWidget(uintptr_t browser) {
    uintptr_t userWidget = findClassByName("UserWidget");
    if (!userWidget) return 0;
    int depth = 0;
    for (uintptr_t o = browser; o; o = get<uintptr_t>(o + L().obj_outer)) {
        if (depth++ < 4) MT_LOG("tablet: browser outer: %s", objName(classOf(o)).c_str());
        if (api->is_a(classOf(o), userWidget)) return o;
    }
    return 0;
}

void toggleTablet(void*) {
    if (g_tablet) {                       // close it
        callRemoveFromParent(g_tablet);
        g_tablet = g_tabletBrowser = 0;
        g_tabletOpen = false;
        MT_LOG("tablet: closed");
        return;
    }
    // The game's own web view: the browser is already wired up, all it needs
    // is its widget on screen.
    uintptr_t live = findLiveBrowser();
    if (live) {
        uintptr_t owner = ownerWidget(live);
        if (owner) {
            callAddToViewport(owner, 1000);
            callLoadURL(live, g_panelUrl());
            g_tablet = owner;
            g_tabletBrowser = live;
            g_tabletOpen = true;
            MT_LOG("tablet: open in the game's own web view (%s)", objName(owner).c_str());
            return;
        }
        MT_LOG("tablet: the game's web browser belongs to no widget");
    }
    uintptr_t pc = findPlayerController();
    if (!pc) { MT_LOG("tablet: no player controller -- the game is not in a world yet"); return; }
    MT_LOG("tablet: player controller %p", (void*)pc);
    // The game's own web widgets, best first: the leaderboard is the one the
    // player is looking at when they ask about reviews and rankings.
    static const char* classes[] = {"LeaderboardWidget_C", "LeaderboardWidget",
                                    "MainMenuWidget_C", "MainMenuWidget",
                                    "TownStateWidget_C", "TownStateWidget"};
    for (const char* name : classes) {
        uintptr_t cls = findClassByName(name);
        if (!cls) { MT_LOG("tablet: no class called %s", name); continue; }
        // The blueprint first: it is the one with a widget tree.
        uintptr_t bp = findSubclass(cls);
        uintptr_t order[2] = {bp, cls};
        for (int k = 0; k < 2; k++) {
            uintptr_t tryCls = order[k];
            if (!tryCls) continue;
            uintptr_t widget = createWidget(tryCls, pc);
            if (!widget) { MT_LOG("tablet: %s: Create returned nothing", objName(tryCls).c_str()); continue; }
            callAddToViewport(widget, 1000);
            uintptr_t browser = widgetBrowser(widget, tryCls);
            if (!browser) { MT_LOG("tablet: %s: created, but it has no web browser", objName(tryCls).c_str()); callRemoveFromParent(widget); continue; }
            callLoadURL(browser, g_panelUrl());
            g_tablet = widget;
            g_tabletBrowser = browser;
            g_tabletOpen = true;
            MT_LOG("tablet: open (%s)", objName(tryCls).c_str());
            return;
        }
    }
    MT_LOG("tablet: none of the game's web widgets could be made -- the panel is at http://localhost:%d/panel", g_cfg.webPort);
}

// The panel: our own page, on our own port, in the player's browser ----
// The game's UI is UMG and its web view belongs to a widget we do not own, so
// the panel is not drawn in the game: it is a page, and CTRL+F9 puts it in
// the player's browser. The port is ours alone and it is local only.
int g_panelPort = 0;

  void serveLocal(SOCKET client) {
    Request req;
    if (!readRequest(client, req)) { closesocket(client); return; }
    std::string answer = apiAnswer(req);
    if (!answer.empty()) {
        answer = httpAnswer(answer);
    } else if (req.target == "/" || req.target == "/panel") {
        std::string page = panelPage();
        answer = page.empty() ? httpAnswer("{\"error\":\"panel missing\"}", "404 Not Found")
                              : httpRaw("200 OK", "text/html; charset=utf-8", page);
    } else {
        answer = httpAnswer("{\"error\":\"not found\"}", "404 Not Found");
    }
    sendAll(client, answer);
    shutdown(client, SD_SEND);
    closesocket(client);
  }

  // The first free port from the one we are told to start at.
void panelServer(int firstPort) {
    for (int port = firstPort; port < firstPort + 32; port++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return;
        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((u_short)port);
        if (bind(s, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR || listen(s, 16) == SOCKET_ERROR) {
            closesocket(s);
            continue;
        }
        g_panelPort = port;
        MT_LOG("panel: http://127.0.0.1:%d/panel", port);
        while (!g_stop) {
            SOCKET client = accept(s, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            std::thread(serveLocal, client).detach();
        }
        closesocket(s);
        return;
    }
    MT_LOG("panel: no free port from %d", firstPort);
}

void openPanel() {
    if (!g_panelPort) { MT_LOG("panel: not up yet"); return; }
    wchar_t url[128];
    swprintf(url, 128, L"http://127.0.0.1:%d/panel", g_panelPort);
    ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    MT_LOG("panel: opened %ls", url);
}

// CTRL+F9, polled on our own thread: the game's input stack is not ours to
// touch, and a key we read ourselves costs nothing.
void keyThread() {
    bool down = false;
    while (!g_stop) {
        bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        if (f9 && ctrl && !down) {
            down = true;
            HWND fg = GetForegroundWindow();
            DWORD pid = 0;
            if (fg) GetWindowThreadProcessId(fg, &pid);
            if (pid == GetCurrentProcessId()) {
                MT_LOG("CTRL+F9 pressed");
                openPanel();
            } else {
                MT_LOG("CTRL+F9 pressed, but the foreground window is not the game (pid %lu)", pid);
            }
        }
        if (!f9) down = false;
        Sleep(30);
    }
}

// Can we reach the real server? If not, the game is better off on its own
// than pointed at a proxy that cannot forward.
bool upstreamOk(const std::wstring& target) {
    Request probe;
    probe.method = "GET";
    probe.target = "/";
    return forward(target, probe, false).rfind("HTTP/1.1 502", 0) != 0;
}

}  // namespace

static DWORD WINAPI run(LPVOID) {
    MT_LOG("MTNet loaded: the game's network layer, in this process");
    selfCheck();
    // On by default: installing the plugin is asking for it. It only listens
    // and points the game at itself -- the traffic it forwards is the game's
    // own, unchanged, until something in [CVars] says otherwise.
    if (iniNum(L"mt_net.ini", L"MTNet", L"Enabled", 1) == 0) {
        MT_LOG("MTNet is off (Enabled=0 in mt_net.ini)");
        return 0;
    }
    g_cfg.port = (int)iniNum(L"mt_net.ini", L"MTNet", L"Port", 81);
    g_cfg.webPort = (int)iniNum(L"mt_net.ini", L"MTNet", L"WebPort", 82);
    std::wstring t = iniGet(L"mt_net.ini", L"MTNet", L"Target");
    std::wstring wt = iniGet(L"mt_net.ini", L"MTNet", L"WebTarget");
    if (!t.empty()) g_cfg.target = t;
    if (!wt.empty()) g_cfg.webTarget = wt;
    g_cfg.log = iniNum(L"mt_net.ini", L"MTNet", L"Log", 0) != 0;
    MT_LOG("host %d -> %ls, web %d -> %ls", g_cfg.port, g_cfg.target.c_str(), g_cfg.webPort, g_cfg.webTarget.c_str());

    bool server = dedicatedServer();
    g_panel = readFile(gameDir() + L"mt_net_panel.html");
    if (g_panel.empty())
        MT_LOG("mt_net_panel.html is missing from the game folder -- no tablet");
    else
        MT_LOG("tablet page loaded (%zu bytes)", g_panel.size());
    if (server) {
        g_password = toNarrow([&] {
            wchar_t b[128] = {0};
            GetPrivateProfileStringW(L"MTNet", L"Password", L"", b, 128, cfgIni().c_str());
            return std::wstring(b);
        }());
        if (g_password.size() < 8) {
            g_password = randomText(20);
            WritePrivateProfileStringW(L"MTNet", L"Password", toWide(g_password).c_str(), cfgIni().c_str());
        }
        g_session = randomText(32);
        MT_LOG("dedicated server -- panel requires the password");
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { MT_LOG("WSAStartup failed -- MTNet is off"); return 0; }
    std::thread web(listener, g_cfg.webPort, g_cfg.webTarget, !server);
    std::thread host(listener, g_cfg.port, g_cfg.target, false);
    std::thread panel;
    if (server) {
        int panelPort = (int)iniNum(L"mt_net.ini", L"MTNet", L"PanelPort", 8091);
        panel = std::thread(panelListener, panelPort);
    }
    // The tablet: the game's own web browser, and the key that opens it.
    static const char* kLoadUrl[] = {"LoadURL"};
    for (int tries = 0; tries < 30 && !api->watch_functions(kLoadUrl, 1, onLoadURL, nullptr); tries++)
        Sleep(1000);
    MT_LOG("CTRL+F9 opens the panel in your browser");
    std::thread(panelServer, (int)iniNum(L"mt_net.ini", L"MTNet", L"UiPort", 8090)).detach();
    std::thread(keyThread).detach();
    if (upstreamOk(g_cfg.target)) {
        writeGameUrls(g_cfg.port, g_cfg.webPort);
        Request cfg;
        cfg.method = "POST";
        cfg.target = "/config";
        cfg.body = "{\"account_id\":0,\"version\":\"0\"}";
        cfg.headers = "content-type: application/json";
        forward(g_cfg.target, cfg, false);   // cache the catalogue for the gear panel
    } else {
        MT_LOG("the real server is not reachable -- the game is left pointing where it was");
    }
    host.join();
    web.join();
    if (panel.joinable()) panel.join();
    WSACleanup();
    return 0;
}

extern "C" __declspec(dllexport) int PluginInit(const MtApi* a) {
    if (!attach(a)) return 1;
    CloseHandle(CreateThread(nullptr, 0, run, nullptr, 0, nullptr));
    return 0;
}
