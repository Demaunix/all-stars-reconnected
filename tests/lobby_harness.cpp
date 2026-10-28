// Drives the runtime's lobby mailbox outside the game, against real Steam (AppID 34190).
// usage: lobby_harness.exe <mod folder> host|search friends|anyone <seconds> [join|joinback]
//   host:     creates a lobby and stays in it for <seconds>.
//   search:   searches once; with "join", joins the first row and stays for the rest of <seconds>;
//             with "joinback", joins and at once posts Leave + Search (the menu's Back while
//             joining): the search must run after the join's answer, and no error may appear.
// Prints every snapshot change. The runtime's own log is <mod folder>\SteamMultiplayer.log.
#include <windows.h>
#include <stdio.h>
#include <string>
#include "mod_api.h"
#include "lobby.h"

static void __cdecl HostLog(const wchar_t* m) { wprintf(L"[host log] %s\n", m); }
static BOOL WINAPI Active() { return TRUE; }

static const wchar_t* StateName(int s) {
    static const wchar_t* n[] = {L"off", L"idle", L"creating", L"searching", L"results", L"joining", L"in lobby", L"error"};
    return s >= 0 && s < 8 ? n[s] : L"?";
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    std::wstring dir = argv[1];
    bool host = wcscmp(argv[2], L"host") == 0;
    int mode = wcscmp(argv[3], L"friends") == 0 ? kModeFriends : kModeAnyone;
    DWORD until = GetTickCount() + _wtoi(argv[4]) * 1000;
    bool joinBack = argc > 5 && wcscmp(argv[5], L"joinback") == 0;
    bool join = joinBack || (argc > 5 && wcscmp(argv[5], L"join") == 0);
    HMODULE m = LoadLibraryExW((dir + L"\\runtime\\SteamMultiplayer.dll").c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { wprintf(L"load failed %lu\n", GetLastError()); return 3; }
    auto init = (SsasrModInitializeFn)GetProcAddress(m, "SsasrModInitialize");
    auto rollback = (SsasrModRollbackFn)GetProcAddress(m, "SsasrModRollback");
    auto post = (int(__cdecl*)(int, int))GetProcAddress(m, "SteamMpTestPost");
    auto read = (void(__cdecl*)(LobbySnapshot*))GetProcAddress(m, "SteamMpTestRead");
    if (!init || !rollback || !post || !read) return 4;
    wchar_t cwd[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, cwd);
    SsasrHostApiV1 api{sizeof(api), 1, GetModuleHandleW(nullptr), cwd, SSASR_HOST_NATIVE_CRT_GATE, GetCurrentThreadId(), 0x248F45, Active, HostLog};
    wprintf(L"init = 0x%08lX\n", (unsigned long)init(&api, dir.c_str()));
    LobbySnapshot s = {};
    uint32_t seen = 0;
    bool posted = false, joinPosted = false;
    while ((int)(until - GetTickCount()) > 0) {
        read(&s);
        if (!posted && s.state == kLobbyIdle) {
            posted = post(host ? kCmdHost : kCmdSearch, mode) != 0;
            wprintf(L"post %s(%s): %s\n", host ? L"host" : L"search", argv[3], posted ? L"ok" : L"refused");
        }
        if (join && !joinPosted && s.state == kLobbyResults && s.rowCount > 0) {
            joinPosted = post(kCmdJoinRow, 0) != 0;
            wprintf(L"post join(row 0): %s\n", joinPosted ? L"ok" : L"refused");
            if (joinBack) {  // the menu's Back while joining: leave, then search again at once
                wprintf(L"post leave: %s\n", post(kCmdLeave, 0) ? L"ok" : L"refused");
                wprintf(L"post search(%s): %s\n", argv[3], post(kCmdSearch, mode) ? L"ok" : L"refused");
            }
        }
        if (s.revision != seen) {
            seen = s.revision;
            wprintf(L"[rev %u] %s error %d mode %d host %d rows %d members %d\n", s.revision, StateName(s.state), s.error, s.mode,
                    s.isHost, s.rowCount, s.memberCount);
            for (int i = 0; i < s.rowCount; ++i)
                wprintf(L"   row %d: %hs (%d/%d) %llu\n", i, s.rows[i].host, s.rows[i].members, s.rows[i].maxMembers, s.rows[i].lobby);
            for (int i = 0; i < s.memberCount; ++i)
                wprintf(L"   member %d: %hs%s%s\n", i, s.members[i].name, s.members[i].isHost ? L" [host]" : L"", s.members[i].isMe ? L" [me]" : L"");
        }
        Sleep(50);
    }
    if (posted && s.state == kLobbyInLobby) wprintf(L"post leave: %s\n", post(kCmdLeave, 0) ? L"ok" : L"refused");
    Sleep(500);
    wprintf(L"rollback = 0x%08lX\n", (unsigned long)rollback());
    return 0;
}
