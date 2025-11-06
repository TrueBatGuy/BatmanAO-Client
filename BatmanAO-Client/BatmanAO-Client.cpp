// BatmanAO-Client.cpp
// Build (Developer Command Prompt for VS):
//   cl /EHsc /O2 BatmanAO-Client.cpp winhttp.lib advapi32.lib shlwapi.lib
// Usage (interactive): just run and follow prompts
//   BatmanAO-Client.exe
// Usage (non-interactive):
//   BatmanAO-Client.exe --server http://your-domain:8385/ [--game "C:\...\Batman Arkham Origins"] [--test]
// http://arkhamorigins.ddns.net:8385/

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <ctime>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Shlwapi.lib")

// ---------- FS utils ----------
static bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool readAll(const std::wstring& path, std::string& out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    out.resize(sz);
    if (sz && fread(&out[0], 1, sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    return true;
}
static bool writeAll(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

// ---------- Registry ----------
static std::wstring getRegStr(HKEY root, const wchar_t* sub, const wchar_t* name) {
    HKEY h{};
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &h) != ERROR_SUCCESS) return L"";
    wchar_t buf[4096]; DWORD type = 0, cb = sizeof(buf);
    LONG r = RegGetValueW(h, nullptr, name, RRF_RT_REG_SZ, &type, buf, &cb);
    RegCloseKey(h);
    if (r == ERROR_SUCCESS) return std::wstring(buf);
    return L"";
}

// ---------- Game path helpers ----------
static std::wstring autoDetectGameDir() {
    // Steam path from registry
    std::wstring steam = getRegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty())
        steam = getRegStr(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        std::wstring g = steam + L"\\steamapps\\common\\Batman Arkham Origins";
        if (dirExists(g)) return g;
    }
    // Common fallbacks
    const wchar_t* guesses[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Batman Arkham Origins",
        L"D:\\Steam\\steamapps\\common\\Batman Arkham Origins",
        L"E:\\Steam\\steamapps\\common\\Batman Arkham Origins",
        nullptr
    };
    for (int i = 0; guesses[i]; ++i) {
        if (dirExists(guesses[i])) return guesses[i];
    }
    return L"";
}
static std::wstring iniPathFromGameDir(const std::wstring& gameDir) {
    return gameDir + L"\\Online\\BmGame\\Config\\DefaultWBIDVars.ini";
}

// ---------- Backup helpers ----------
static std::wstring nowStamp() {
    wchar_t buf[64];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    swprintf(buf, 64, L"%04d%02d%02d_%02d%02d%02d",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}
static bool backupFile(const std::wstring& src) {
    std::wstring dst = src + L".bak." + nowStamp();
    if (!CopyFileW(src.c_str(), dst.c_str(), TRUE)) {
        // fallback static .bak
        dst = src + L".bak";
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) return false;
    }
    return true;
}

// ---------- String helpers ----------
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}
static std::string normalizeBaseUrl(std::string u) {
    u = trim(u);
    if (u.empty()) return u;
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) {
        u = "http://" + u;
    }
    if (u.back() != '/') u.push_back('/');
    return u;
}
static bool startsWithInsensitive(const std::string& s, const std::string& pref) {
    if (s.size() < pref.size()) return false;
    for (size_t i = 0; i < pref.size(); ++i) {
        char a = s[i], b = pref[i];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// ---------- INI read/compare helpers for idempotency ----------
static std::string normalizeUrlForCompare(std::string u) {
    u = trim(u);
    if (u.empty()) return u;
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) u = "http://" + u;
    if (u.back() != '/') u.push_back('/');
    return u;
}
static bool parseIniValue(const std::string& line, const std::string& keyLower, std::string& outVal) {
    auto trimLocal = [](const std::string& s)->std::string {
        size_t a = s.find_first_not_of(" \t\r\n");
        size_t b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        return s.substr(a, b - a + 1);
        };

    std::string l = line;
    for (auto& c : l) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

    size_t eq = l.find('=');
    if (eq == std::string::npos) return false;
    std::string k = trimLocal(l.substr(0, eq));
    if (k != keyLower) return false;

    std::string rawVal = line.substr(eq + 1);
    rawVal = trimLocal(rawVal);
    if (!rawVal.empty() && (rawVal.front() == '"' || rawVal.front() == '\'')) rawVal.erase(0, 1);
    if (!rawVal.empty() && (rawVal.back() == '"' || rawVal.back() == '\'')) rawVal.pop_back();
    outVal = trimLocal(rawVal);
    return true;
}
// true, если все keys уже указывают на baseUrl в INI
static bool iniAlreadyConfigured(const std::wstring& iniPath, const std::string& baseUrlIn,
    const std::vector<std::string>& keys) {
    std::string text;
    if (!readAll(iniPath, text)) return false;

    std::string want = normalizeUrlForCompare(baseUrlIn);

    std::vector<std::string> keysLower;
    keysLower.reserve(keys.size());
    for (auto& k : keys) {
        std::string kl = k;
        for (auto& c : kl) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        keysLower.push_back(kl);
    }

    std::vector<bool> seen(keys.size(), false);

    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        size_t len = (e == std::string::npos) ? (text.size() - i) : (e - i + 1);
        std::string raw = text.substr(i, len);
        std::string line = raw;
        // strip EOL
        if (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        }

        for (size_t k = 0; k < keysLower.size(); ++k) {
            std::string val;
            if (parseIniValue(line, keysLower[k], val)) {
                seen[k] = true;
                if (normalizeUrlForCompare(val) != want) return false; // другой URL
                break;
            }
        }

        if (e == std::string::npos) break;
        i = e + 1;
    }

    for (bool s : seen) if (!s) return false; // отсутствует хотя бы один ключ
    return true;
}

// ---------- INI patcher (multiple keys) ----------
static bool patchIniKeys(const std::wstring& iniPath,
    const std::string& baseUrlIn,
    const std::vector<std::string>& keys,
    std::vector<std::string>* changedOut = nullptr,
    std::vector<std::string>* addedOut = nullptr)
{
    std::string text;
    if (!readAll(iniPath, text)) return false;

    // Global EOL (based on the first newline)
    std::string fileEol = "\n";
    {
        size_t p = text.find('\n');
        if (p != std::string::npos && p > 0 && text[p - 1] == '\r') fileEol = "\r\n";
    }

    std::string baseUrl = baseUrlIn; // already normalized by caller
    std::string out; out.reserve(text.size() + 256);

    std::vector<bool> seen(keys.size(), false);
    std::vector<std::string> changed, added;

    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        size_t len = (e == std::string::npos) ? (text.size() - i) : (e - i + 1);
        std::string raw = text.substr(i, len);

        // EOL of this line
        std::string lineEol = "";
        if (!raw.empty() && raw.back() == '\n') {
            lineEol = (raw.size() >= 2 && raw[raw.size() - 2] == '\r') ? "\r\n" : "\n";
        }

        // Content without EOL
        std::string line = raw;
        if (!lineEol.empty()) line.erase(line.size() - lineEol.size());

        std::string s = trim(line);
        bool replacedThisLine = false;

        for (size_t k = 0; k < keys.size(); ++k) {
            const std::string& key = keys[k];
            if (startsWithInsensitive(s, key + "=")) {
                out += key + "=\"" + baseUrl + "\"" + lineEol;
                seen[k] = true;
                changed.push_back(key);
                replacedThisLine = true;
                break;
            }
        }

        if (!replacedThisLine) {
            out += raw; // keep as-is
        }

        if (e == std::string::npos) break;
        i = e + 1;
    }

    // Append any missing keys at the end
    for (size_t k = 0; k < keys.size(); ++k) {
        if (!seen[k]) {
            if (!out.empty() && (out.back() != '\n' && out.back() != '\r')) out += fileEol;
            out += keys[k] + "=\"" + baseUrl + "\"" + fileEol;
            added.push_back(keys[k]);
        }
    }

    if (changedOut) *changedOut = changed;
    if (addedOut) *addedOut = added;

    // If nothing actually changed — avoid writing
    if (out == text) {
        if (changedOut) changedOut->clear();
        if (addedOut) addedOut->clear();
        return true;
    }

    return writeAll(iniPath, out);
}

// ---------- HTTP probe ----------
static bool parseHostPortPath(const std::string& url, std::wstring& host, INTERNET_PORT& port, std::wstring& path) {
    std::string u = url;
    if (u.rfind("http://", 0) == 0) {
        u.erase(0, 7);
        port = 80;
    }
    else if (u.rfind("https://", 0) == 0) {
        u.erase(0, 8);
        port = 443;
    }
    else return false;

    size_t slash = u.find('/');
    std::string hostport = (slash == std::string::npos) ? u : u.substr(0, slash);
    std::string pth = (slash == std::string::npos) ? "/" : u.substr(slash);

    size_t colon = hostport.find(':');
    std::string h = (colon == std::string::npos) ? hostport : hostport.substr(0, colon);
    if (colon != std::string::npos) {
        port = (INTERNET_PORT)atoi(hostport.substr(colon + 1).c_str());
        if (!port) port = 80;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, h.c_str(), -1, nullptr, 0);
    std::wstring wh(wlen ? wlen - 1 : 0, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, h.c_str(), -1, &wh[0], wlen);

    wlen = MultiByteToWideChar(CP_UTF8, 0, pth.c_str(), -1, nullptr, 0);
    std::wstring wp(wlen ? wlen - 1 : 0, L'\0');
    if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, pth.c_str(), -1, &wp[0], wlen);

    host = wh; path = wp;
    return true;
}
static bool httpGetJson(const std::string& baseUrl, const std::string& rel, bool httpsOk = true) {
    std::string full = baseUrl;
    if (!full.empty() && full.back() == '/' && !rel.empty() && rel.front() == '/')
        full.pop_back();
    full += rel;

    std::wstring host, path;
    INTERNET_PORT port = 0;
    if (!parseHostPortPath(full, host, port, path)) return false;

    bool isHttps = (full.rfind("https://", 0) == 0);
    HINTERNET hSes = WinHttpOpen(L"AOClientPatcher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSes) return false;

    HINTERNET hCon = WinHttpConnect(hSes, host.c_str(), port, 0);
    if (!hCon) { WinHttpCloseHandle(hSes); return false; }

    HINTERNET hReq = WinHttpOpenRequest(hCon, L"GET", path.c_str(), NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        isHttps ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) { WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes); return false; }

    BOOL ok = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, 0, 0, 0, 0)
        && WinHttpReceiveResponse(hReq, NULL);
    if (!ok) {
        WinHttpCloseHandle(hReq); WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
        return false;
    }
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sz, NULL);

    WinHttpCloseHandle(hReq); WinHttpCloseHandle(hCon); WinHttpCloseHandle(hSes);
    return (status >= 200 && status < 400);
}

// Пауза при выходе, чтобы окно не закрывалось сразу
static void PauseOnExit() {
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode = 0;
    GetConsoleMode(h, &oldMode);

    // Включаем line input + echo input, чтобы точно поймать новый Enter
    SetConsoleMode(h, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

    printf("\nPress Enter to exit...");
    fflush(stdout);

    // Ждём РОВНО один новый Enter (предыдущий \n нас уже не волнует)
    DWORD read = 0;
    char dummy[2];
    ReadConsoleA(h, dummy, 1, &read, NULL);

    // Восстановим режим
    SetConsoleMode(h, oldMode);
}

// ---------- Cosmetics ----------
void PrintBatBar() {
    printf(
        "\n"
        "                      _.-/                                                \\-._\n"
        "                    ./  /                                                  \\  \\.\n"
        "                  ./    \\.                    /\\    /\\                    ./    \\.\n"
        "                ./       \\.                  /  \\__/  \\                  ./       \\.\n"
        "              ./           \\___             /          \\             ___/           \\.\n"
        "            ./                 ---______---/            \\---______---                 \\.\n"
        "          ./                                                                            \\.\n"
        "         /                                                                                \\ \n"
        "        /                                                                                  \\\n"
        "       /.                                                                                  .\\\n"
        "       /          ___                                                          ___          \\ \n"
        "      /     ___---   ---___  ___                                    ___  ___---   ---___     \\\n"
        "     /  _---               \\/   ---___                        ___---   \\/               ---_  \\\n"
        "     /./                              ---___            ___---                              \\.\\\n"
        "     //                                     ---.    .---                                     \\\\\n"
        "                                               \\    /\n"
        "                                                \\  /\n"
        "                                                 \\/\n"
        "\n                            C O N N E C T E D    T O    B A T C O M P U T E R \n\n"
    );
}

// ---------- Entry ----------
int wmain(int argc, wchar_t** wargv) 
{
    atexit(PauseOnExit);
    SetConsoleOutputCP(CP_UTF8);

    std::wstring gameDir;
    std::string server;

    if (gameDir.empty()) {
        gameDir = autoDetectGameDir();
    }
    if (gameDir.empty() || !dirExists(gameDir)) {
        wprintf(L"Unable to find game folder. Please enter the path to \"Batman Arkham Origins\":\n> ");
        wchar_t buf[MAX_PATH * 4]; if (!fgetws(buf, MAX_PATH * 4, stdin)) return 1;
        std::wstring path = buf; while (!path.empty() && (path.back() == L'\n' || path.back() == L'\r')) path.pop_back();
        if (!dirExists(path)) { wprintf(L"The folder does not exist.\n"); return 1; }
        gameDir = path;
    }

    std::wstring ini = iniPathFromGameDir(gameDir);
    if (!fileExists(ini)) {
        wprintf(L"INI not found: %s\nExpected: Online\\BmGame\\Config\\DefaultWBIDVars.ini\n", ini.c_str());
        return 1;
    }

    if (server.empty()) {
        printf("Enter the server address (example: http://your-domain:8385/ or your-domain:8385):\n> ");
        char buf[1024]; if (!fgets(buf, sizeof(buf), stdin)) return 1;
        server = buf;
    }
    server = normalizeBaseUrl(server);
    if (server.empty()) { printf("Empty address.\n"); return 1; }

    // Probe server (optional)
    printf("Server check: %sstore/catalog/general ...\n", server.c_str());
    bool ok = httpGetJson(server, "/store/catalog/general");
    if (!ok) {
        printf("Warning: The server did not respond with 2xx/3xx. Continue patch? [y/N]: ");
        int c = getchar(); if (c != 'y' && c != 'Y') return 2;
        while (c != '\n' && c != EOF) c = getchar();
    }
    else {
        printf("OK: The server is responding.\n");
        PrintBatBar();
    }

    // Idempotency: if already configured for this server — do nothing
    std::vector<std::string> keysToPatch = {
        "BaseUrl"
    };
    if (iniAlreadyConfigured(ini, server, keysToPatch)) {
        wprintf(L"Already configured for -> %S\nINI: %s\n", server.c_str(), ini.c_str());
        printf("Nothing to change. Launch the game.\n");
        return 0;
    }

    // Backup INI (only if we will actually change it)
    if (!backupFile(ini)) {
        wprintf(L"Warning: Failed to create backup INI.\n");
    }

    // Patch multiple keys
    std::vector<std::string> changed, added;
    if (!patchIniKeys(ini, server, keysToPatch, &changed, &added)) {
        wprintf(L"Error: Failed to write INI.\n");
        return 3;
    }

    // Report
    wprintf(L"Done! Updated values -> %S\nINI: %s\n", server.c_str(), ini.c_str());
    if (!changed.empty()) {
        printf("Changed: ");
        for (size_t i = 0; i < changed.size(); ++i) {
            if (i) printf(", ");
            printf("%s", changed[i].c_str());
        }
        printf("\n");
    }
    if (!added.empty()) {
        printf("Added: ");
        for (size_t i = 0; i < added.size(); ++i) {
            if (i) printf(", ");
            printf("%s", added[i].c_str());
        }
        printf("\n");
    }

    printf("Launch the game. To restore, replace the INI with the .bak file created next to it.\n");
    return 0;
}
