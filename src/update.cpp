// The auto-update in the game (notes/AUTO-UPDATE.md): one background thread per launch. It waits until the game has
// started and (config.ini [Update] Auto=1, the default; release builds only; a ChaoGarage with "pending-mod-updates")
// fetches GitHub's latest update.txt + signature, and if a newer signed release is there, its stored zip; then
// src/update.h checks and STAGES it in mods-updates\staged with its ready marker. It never moves the mod's folder: the
// loader keeps the payloads open while the game runs, so ChaoGarage puts the staging in place at the next start.
// Every failure is one log line and changes nothing in mods\. Writes only in mods-updates\.
#include <windows.h>
#include <winhttp.h>
#include <atomic>
#include <string>

#include "log.h"
#include "update.h"
#ifdef UPDATE_TEST_BUILD
#include "update_test_key.h"  // kUpdateTestKeys[1][64]: tests/update_e2e.py writes it into its own build folder
#endif

namespace {

const wchar_t kRepoPath[] = L"/Demaunix/all-stars-reconnected/releases/";
const DWORD kStartDelayMs = 15000;  // let the game reach its menus first

// ChaoGarage's loader info (ChaoGarage src/common/loader_info.h, 1.1.0 and later); only the fields we read.
struct LoaderInfoV1 {
    uint32_t size, version;
    const wchar_t* release;
    uint32_t bootstrapAbi, hostAbi, modIniSchema, planSchema;
    const char* const* capabilities;
};
typedef const LoaderInfoV1*(__cdecl* GetLoaderInfoFn)(void);

HANDLE g_thread = nullptr;
HANDLE g_stop = nullptr;
std::atomic<HINTERNET> g_request{nullptr};  // closed by AutoUpdateStop to cut a download short
std::wstring g_modDir, g_from;               // g_from: [Debug] UpdateFrom (a local folder instead of GitHub)
std::string g_running;
bool g_auto = true;
std::atomic<bool> g_noticeReady{false}, g_noticeApplied{false};  // the main menu's notices (AutoUpdateNoticePending)
std::wstring g_updatesRoot;  // <game>\mods-updates, or empty when this copy isn't <game>\mods\Steam Multiplayer

// <game>\mods-updates for this copy, or empty when it isn't <game>\mods\Steam Multiplayer (a test copy, a renamed folder).
std::wstring UpdatesRoot(const std::wstring& modDir) {
    size_t cut = modDir.find_last_of(L'\\');
    std::wstring mods = cut == std::wstring::npos ? L"" : modDir.substr(0, cut);
    size_t cut2 = mods.find_last_of(L'\\');
    if (cut == std::wstring::npos || cut2 == std::wstring::npos || _wcsicmp(modDir.c_str() + cut + 1, L"Steam Multiplayer") != 0 ||
        _wcsicmp(mods.c_str() + cut2 + 1, L"mods") != 0)
        return {};
    return mods.substr(0, cut2) + L"\\mods-updates";
}

bool Stopping(DWORD waitMs = 0) { return WaitForSingleObject(g_stop, waitMs) == WAIT_OBJECT_0; }

std::wstring Encode(const std::string& name) {  // a URL path part (names are checked ASCII)
    std::wstring out;
    for (unsigned char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_') {
            out += (wchar_t)c;
        } else {
            wchar_t hex[4];
            swprintf_s(hex, L"%%%02X", c);
            out += hex;
        }
    }
    return out;
}

bool AllowedHost(const std::wstring& host) {
    size_t n = host.size();
    const wchar_t suffix[] = L".githubusercontent.com";
    size_t s = wcslen(suffix);
    return _wcsicmp(host.c_str(), L"github.com") == 0 || (n > s && _wcsicmp(host.c_str() + n - s, suffix) == 0);
}

// GET https://github.com<path>, following GitHub's redirects (HTTPS only, GitHub hosts only). `sink` takes each piece;
// at most `limit` bytes. (Everything fetched is checked afterwards by signature or hash; the host rule is extra care.)
const char* Fetch(const std::wstring& path, uint64_t limit, const std::function<bool(const void*, DWORD)>& sink, uint64_t& total) {
    total = 0;
    std::wstring agent = L"AllStarsReconnected/" + std::wstring(g_running.begin(), g_running.end());
    HINTERNET session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return "the internet functions can't start";
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    HINTERNET connect = WinHttpConnect(session, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                                : nullptr;
    const char* why = nullptr;
    if (!request) why = "GitHub can't be reached";
    if (request) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        g_request = request;
        if (Stopping()) why = "stopped";
        else if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                 !WinHttpReceiveResponse(request, nullptr))
            why = "GitHub didn't answer (offline?)";
    }
    if (!why) {
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                            WINHTTP_NO_HEADER_INDEX);
        wchar_t url[2048] = {}, host[256] = {};
        DWORD urlLen = sizeof(url);
        URL_COMPONENTS uc = {sizeof(uc)};
        uc.lpszHostName = host;
        uc.dwHostNameLength = 256;
        if (status == 404) why = "no release there (404)";
        else if (status != 200) why = "GitHub answered with an error";
        else if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, url, &urlLen) || !WinHttpCrackUrl(url, 0, 0, &uc) ||
                 uc.nScheme != INTERNET_SCHEME_HTTPS || !AllowedHost(host))
            why = "GitHub redirected somewhere unexpected";
    }
    std::vector<char> buf(1 << 16);
    while (!why) {
        DWORD got = 0;
        if (Stopping()) why = "stopped";
        else if (!WinHttpReadData(request, buf.data(), (DWORD)buf.size(), &got)) why = "the download broke off";
        else if (!got) break;
        else if ((total += got) > limit) why = "the download is bigger than announced";
        else if (!sink(buf.data(), got)) why = "the download can't be saved";
    }
    HINTERNET mine = request;
    if (request && g_request.compare_exchange_strong(mine, nullptr)) WinHttpCloseHandle(request);  // (else AutoUpdateStop closed it)
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return why;
}

// update.txt or its signature: from GitHub (the latest release, or a given tag) or the [Debug] UpdateFrom folder.
const char* FetchSmall(const std::wstring& name, uint64_t limit, std::string& out) {
    out.clear();
    if (!g_from.empty()) {
        HANDLE h = CreateFileW((g_from + L"\\" + name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return "the UpdateFrom folder lacks the file";
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n && out.size() <= limit) out.append(buf, n);
        CloseHandle(h);
        return out.size() > limit ? "the file is too big" : nullptr;
    }
    uint64_t total = 0;
    return Fetch(kRepoPath + std::wstring(L"latest/download/") + name, limit, [&](const void* p, DWORD n) {
        out.append((const char*)p, n);
        return true;
    }, total);
}

class FileReader {
public:
    explicit FileReader(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    ~FileReader() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    UpdateReader Reader() {
        return [this](uint64_t off, void* buf, size_t n) {
            LARGE_INTEGER at;
            at.QuadPart = (LONGLONG)off;
            DWORD got = 0;
            return SetFilePointerEx(h_, at, nullptr, FILE_BEGIN) && ReadFile(h_, buf, (DWORD)n, &got, nullptr) && got == n;
        };
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

// The running ChaoGarage's info, or null (a loader before 1.1.0, or none).
const LoaderInfoV1* Loader() {
    HMODULE loader = GetModuleHandleW(L"ChaoGarage.dll");
    GetLoaderInfoFn info = loader ? (GetLoaderInfoFn)GetProcAddress(loader, "ChaoGarageGetLoaderInfo") : nullptr;
    const LoaderInfoV1* li = info ? info() : nullptr;
    return li && li->size >= sizeof(LoaderInfoV1) ? li : nullptr;
}

// The release's signature: one of the built-in keys (0 current, 1 spare), or -1. Test builds also take the end-to-end
// test's own key (build/e2e, made at test time), never a release build.
int SignedBy(const std::string& text, const std::string& sig) {
    int key = UpdateSignedBy(text, (const uint8_t*)sig.data(), sig.size());
#ifdef UPDATE_TEST_BUILD
    if (key < 0 && UpdateSignedBy(text, (const uint8_t*)sig.data(), sig.size(), kUpdateTestKeys, 1) == 0) key = 2;
#endif
    return key;
}

void Run(HANDLE& lock) {
    // Where we are: <game>\mods\Steam Multiplayer. Anything else (a test copy, a renamed folder): no updates.
    if (g_updatesRoot.empty()) {
        Log(false, L"auto-update: off (this copy isn't <game>\\mods\\Steam Multiplayer)");
        return;
    }
    UpdatePaths p;
    p.live = g_modDir;
    p.root = g_updatesRoot;
    if (Stopping(kStartDelayMs)) return;
    if (!g_auto) {
        Log(false, L"auto-update: off (config.ini [Update] Auto=0)");
        return;
    }
    // Developer builds never update themselves; with [Debug] UpdateFrom the author tests one as its release number.
    std::string running = g_running;
    if (!g_from.empty() && running.find('-') != std::string::npos) running = running.substr(0, running.find('-'));
    if (!UpdateIsRelease(running)) {
        Log(false, L"auto-update: off for this developer build (%hs)", g_running.c_str());
        return;
    }
    if (!UpdateExists(p.root) && !UpdateMakeDirs(p.root)) {
        Log(false, L"auto-update: can't make %s (error %lu)", p.root.c_str(), GetLastError());
        return;
    }
    // A link at mods-updates or its staging folder (ChaoGarage would never read through it either): no updates.
    for (const std::wstring& dir : {p.root, p.root + L"\\staged"}) {
        DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))) {
            Log(true, L"auto-update: off (%s is a link or a file, not a folder)", dir.c_str());
            return;
        }
    }
    // One game at a time per game folder, and the lock before any clean-up (another game may be staging).
    // (never through a link: a planted link's target would be emptied and deleted)
    DWORD la = GetFileAttributesW((p.root + L"\\lock").c_str());
    lock = la != INVALID_FILE_ATTRIBUTES && (la & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
               ? INVALID_HANDLE_VALUE
               : CreateFileW((p.root + L"\\lock").c_str(), GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                             FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (lock == INVALID_HANDLE_VALUE) {
        Log(false, L"auto-update: another game is checking (skipped)");
        return;
    }
    const LoaderInfoV1* li = Loader();
    const char* applyCap[] = {kUpdateLoaderCapability, nullptr};
    std::vector<std::string> need(applyCap, applyCap + 1);
    bool loaderApplies = li && UpdateHasCaps(need, li->capabilities);
    if (UpdateCleanUp(p)) {
        if (loaderApplies) {
            Log(false, L"auto-update: a downloaded update is waiting; ChaoGarage puts it in place at the next game start");
            return;
        }
        // This ChaoGarage can't put it in place (an older one now): drop it, so it never waits forever.
        DeleteFileW(p.Ready().c_str());
        UpdateRemoveTree(p.Staged());
        Log(false, L"auto-update: a waiting update was dropped (this ChaoGarage can't put it in place)");
    }
    // Windows' own internet functions only (never a winhttp.dll from the game folder): loaded here before any WinHTTP call,
    // so the delayed import finds this module.
    if (g_from.empty() && !LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        Log(false, L"auto-update: Windows' internet functions aren't available (error %lu)", GetLastError());
        return;
    }
    std::string text, sig;
    const char* why = FetchSmall(L"update.txt", kUpdateMaxManifest, text);
    if (!why) why = FetchSmall(L"update.txt.sig", 64, sig);
    UpdateManifest m;
    int key = -1;
    if (!why && (key = SignedBy(text, sig)) < 0) why = "the release's signature is not ours (refused)";
    if (!why) why = UpdateParse(text, m);
    if (why) {
        Log(false, L"auto-update: no update (%hs)", why);
        return;
    }
    if (!UpdateNewer(m.version, running)) {
        Log(false, L"auto-update: up to date (%hs; the latest release is %hs)", g_running.c_str(), m.version.c_str());
        return;
    }
    // The download only when this ChaoGarage can put an update in place, and the release can run under it.
    if (!loaderApplies || !UpdateHasCaps(m.loaderCaps, li->capabilities)) {
        Log(true, L"auto-update: %hs is out (you have %hs). ChaoGarage %s can't install it by itself: get ChaoGarage 1.3.0 or newer, "
                  L"or the mod from its GameBanana page", m.version.c_str(), g_running.c_str(), li && li->release ? li->release : L"(before 1.1.0)");
        return;
    }
    Log(true, L"auto-update: %hs is out (you have %hs); downloading %hs (%llu bytes)", m.version.c_str(), g_running.c_str(),
        m.zipName.c_str(), m.zipSize);
    std::wstring download = p.root + L"\\download", zipPath;
    if (!g_from.empty()) {
        zipPath = g_from + L"\\" + UpdateWide(m.zipName);
    } else {
        zipPath = download + L"\\" + UpdateWide(m.zipName) + L".part";
        HANDLE out = UpdateMakeDirs(download)
                         ? CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)
                         : INVALID_HANDLE_VALUE;
        if (out == INVALID_HANDLE_VALUE) why = "the download folder can't be written";
        uint64_t total = 0;
        if (!why) {
            std::wstring tag = L"download/v" + std::wstring(m.version.begin(), m.version.end()) + L"/" + Encode(m.zipName);
            why = Fetch(kRepoPath + tag, m.zipSize, [&](const void* d, DWORD n) {
                DWORD w = 0;
                return WriteFile(out, d, n, &w, nullptr) && w == n;
            }, total);
            if (!why && total != m.zipSize) why = "the download's size differs from the release's";
        }
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    }
    // The whole zip's hash first (the file stays open share-read: nobody can change it meanwhile), then each file's.
    FileReader file(zipPath);
    if (!why && !file.Ok()) why = "the downloaded zip can't be opened";
    if (!why) {
        UpdateSha256 sha;
        std::vector<uint8_t> buf(1 << 16);
        auto read = file.Reader();
        for (uint64_t at = 0; !why && at < m.zipSize; at += buf.size()) {
            size_t n = (size_t)(m.zipSize - at < buf.size() ? m.zipSize - at : buf.size());
            if (Stopping()) why = "stopped";
            else if (!read(at, buf.data(), n)) why = "the downloaded zip can't be read";
            else sha.Add(buf.data(), n);
        }
        uint8_t got[32];
        if (!why && (!sha.Finish(got) || memcmp(got, m.zipSha, 32) != 0)) why = "the downloaded zip's SHA-256 differs from the release's";
    }
    std::vector<UpdateZipEntry> entries;
    if (!why) why = UpdateReadZip(m.zipSize, file.Reader(), entries);
    if (!why) why = UpdateStageForLoader(m, entries, file.Reader(), p, [] { return Stopping(); });
    if (why) {
        Log(true, L"auto-update: %hs was NOT downloaded (%hs); this version stays", m.version.c_str(), why);
        UpdateRemoveTree(p.Staged());
    } else {
        Log(true, L"auto-update: %hs is downloaded and checked (signed with the %s key); ChaoGarage puts it in place at the next "
                  L"game start", m.version.c_str(), key == 0 ? L"current" : key == 1 ? L"spare" : L"TEST");
        g_noticeReady = true;  // the main menu says so (restart to use it)
    }
    if (g_from.empty()) UpdateRemoveTree(download);
}

// C++ errors (out of memory in this 32-bit process) and Windows exceptions (a failed delayed import) end the check, never the
// game.
void RunGuarded(HANDLE& lock) {
    try {
        Run(lock);
    } catch (...) {
        Log(false, L"auto-update: stopped by an internal error (nothing changed in mods\\)");
    }
}

DWORD WINAPI Thread(void*) {
    HANDLE lock = INVALID_HANDLE_VALUE;
    __try {
        RunGuarded(lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log(false, L"auto-update: stopped by a Windows exception 0x%08lx (nothing changed in mods\\)", GetExceptionCode());
    }
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    return 0;
}

}  // namespace

void AutoUpdateStart(const std::wstring& modDir, const char* runningVersion) {
    g_modDir = modDir;
    g_running = runningVersion;
    std::wstring ini = modDir + L"\\config.ini";
    g_auto = GetPrivateProfileIntW(L"Update", L"Auto", 1, ini.c_str()) != 0;
    wchar_t from[MAX_PATH] = {};
    GetPrivateProfileStringW(L"Debug", L"UpdateFrom", L"", from, MAX_PATH, ini.c_str());
    g_from = from;
    if (!g_from.empty()) Log(true, L"debugging: the auto-update reads %s instead of GitHub (config.ini [Debug] UpdateFrom)", from);
    g_updatesRoot = UpdatesRoot(modDir);
    // The version ChaoGarage just put in place: the main menu says so, once per version (notice.txt).
    if (!g_updatesRoot.empty() && UpdateJustApplied(g_updatesRoot, g_running)) {
        g_noticeApplied = true;
        Log(true, L"auto-update: this start runs %hs, put in place by ChaoGarage (the main menu says so once)", g_running.c_str());
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 0, Thread, nullptr, 0, nullptr) : nullptr;
    if (g_thread) SetThreadPriority(g_thread, THREAD_PRIORITY_BELOW_NORMAL);
    else Log(false, L"auto-update: the thread can't start (error %lu)", GetLastError());
}

int AutoUpdateNoticePending() {
    return g_noticeApplied ? kUpdateNoticeApplied : g_noticeReady ? kUpdateNoticeReady : 0;
}

void AutoUpdateNoticeShown(int kind) {
    if (kind == kUpdateNoticeApplied) {
        g_noticeApplied = false;
        if (!UpdateNoticeMark(g_updatesRoot, g_running)) Log(false, L"auto-update: notice.txt can't be written (the notice may show again)");
    } else if (kind == kUpdateNoticeReady) {
        g_noticeReady = false;
    }
    Log(false, L"auto-update: the main menu shows the %s notice", kind == kUpdateNoticeApplied ? L"updated" : L"restart");
}

// At rollback: stop waiting, cut a running download short, and wait for the thread (it stops at its next step). If it
// doesn't stop in time, this DLL is pinned so it is never unloaded under the thread.
void AutoUpdateStop() {
    if (!g_thread) return;
    SetEvent(g_stop);
    if (HINTERNET r = g_request.exchange(nullptr)) WinHttpCloseHandle(r);
    if (WaitForSingleObject(g_thread, 20000) == WAIT_OBJECT_0) {
        CloseHandle(g_thread);
        g_thread = nullptr;
        return;
    }
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)&AutoUpdateStop, &self);
    Log(false, L"auto-update: still busy at rollback; the mod's DLL stays loaded");
}
