// Offline test of src/config_merge.h: adding the option lines a kept config.ini lacks.
// Build: see tests/run_offline.ps1 (needs build\config_template_embed.h from tools\config_embed.py).
#include <windows.h>
#include <stdio.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "config_merge.h"
#include "config_template_embed.h"

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

static std::string ReadAll(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static std::string Replace(std::string s, const std::string& from, const std::string& to) {
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) s.replace(at, from.size(), to);
    return s;
}

static std::string ToCrlf(const std::string& s) { return Replace(Replace(s, "\r\n", "\n"), "\n", "\r\n"); }

// The value Windows reads from this text for [sec] key (through a real file, as the game does).
static std::string WinRead(const std::string& text, const char* sec, const char* key) {
    char dir[MAX_PATH], path[MAX_PATH];
    GetTempPathA(MAX_PATH, dir);
    GetTempFileNameA(dir, "cfm", 0, path);
    { std::ofstream f(path, std::ios::binary); f << text; }
    char buf[256] = {};
    GetPrivateProfileStringA(sec, key, "<none>", buf, sizeof buf, path);
    DeleteFileA(path);
    return buf;
}

// Every [Section] Key of the template, with its value.
struct Opt { std::string sec, key, value; };
static std::vector<Opt> TemplateOptions() {
    std::vector<Opt> out;
    std::string sec;
    for (auto& raw : config_merge::SplitLines(kConfigTemplate)) {
        std::string t = config_merge::Trim(raw);
        if (!t.empty() && t[0] == '[') sec = t.substr(1, t.find(']') - 1);
        else if (!config_merge::KeyOf(raw).empty()) {
            size_t eq = t.find('=');
            out.push_back(Opt{sec, config_merge::Trim(t.substr(0, eq)), config_merge::Trim(t.substr(eq + 1))});
        }
    }
    return out;
}

static bool AllOptionsReadable(const std::string& text) {  // every template option is read by Windows with its template value
    for (auto& o : TemplateOptions())
        if (WinRead(text, o.sec.c_str(), o.key.c_str()) != o.value) {
            printf("  [%s] %s reads \"%s\", not \"%s\"\n", o.sec.c_str(), o.key.c_str(), WinRead(text, o.sec.c_str(), o.key.c_str()).c_str(), o.value.c_str());
            return false;
        }
    return true;
}

static int Count(const std::string& s, const std::string& what) {
    int n = 0;
    for (size_t at = 0; (at = s.find(what, at)) != std::string::npos; at += what.size()) ++n;
    return n;
}

static bool NoBareLf(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) if (s[i] == '\n' && (i == 0 || s[i - 1] != '\r')) return false;
    return true;
}

int main(int argc, char** argv) {
    const char* fixture = argc > 1 ? argv[1] : "tests\\data\\config_merge\\config-1.1.1.ini";
    std::string tmpl = kConfigTemplate, out;
    std::vector<std::string> added;

    // The shipped values are the code's defaults (so an added line changes nothing in play). Update both together.
    struct Def { const char *sec, *key, *value; } defs[] = {
        {"Background", "RunWhenUnfocused", "1"}, {"Menus", "Enabled", "1"}, {"Menus", "InviteKey", "A"},
        {"Menus", "PadPictures", "auto"}, {"Race", "HostRunsAI", "1"}, {"Race", "ItemSync", "1"},
        {"Voice", "Mode", "ptt"}, {"Voice", "Key", "V"}, {"Voice", "Volume", "100"}, {"Voice", "Muted", ""}, {"Update", "Auto", "1"}};
    auto opts = TemplateOptions();
    bool same = opts.size() == sizeof defs / sizeof defs[0];
    for (size_t i = 0; same && i < opts.size(); ++i)
        same = opts[i].sec == defs[i].sec && opts[i].key == defs[i].key && opts[i].value == defs[i].value;
    Check(same, "the shipped config.ini has exactly the known options, each with the code's default");

    Check(!config_merge::ConfigMerge(tmpl, tmpl, out) && out.empty(), "the shipped file itself: nothing to add");
    Check(!config_merge::ConfigMerge(tmpl, ToCrlf(tmpl), out), "the shipped file with CRLF: nothing to add");

    // The real 1.1.1 config.ini a player kept through the auto-update, with the player's own changes, in CRLF.
    std::string old = Replace(ReadAll(fixture), "\r\n", "\n");  // (a checkout may give it CRLF; the cases pick their own)
    Check(!old.empty() && old.find("PadPictures") == std::string::npos, "fixture: the 1.1.1 config.ini (no PadPictures)");
    std::string player = ToCrlf(Replace(Replace(old, "Key=V", "Key=B"), "RunWhenUnfocused=1", "RunWhenUnfocused=0")) +
                         "\r\n[Debug]\r\nTestPlayers=2\r\n";
    Check(config_merge::ConfigMerge(tmpl, player, out, &added), "1.1.1 file: options added");
    Check(added.size() == 1 && added[0] == "[Menus] PadPictures", "1.1.1 file: exactly PadPictures");
    Check(NoBareLf(out), "1.1.1 file: CRLF kept everywhere");
    Check(WinRead(out, "Menus", "PadPictures") == "auto", "1.1.1 file: Windows reads the added option");
    Check(WinRead(out, "Voice", "Key") == "B" && WinRead(out, "Background", "RunWhenUnfocused") == "0" &&
          WinRead(out, "Debug", "TestPlayers") == "2", "1.1.1 file: the player's own values and [Debug] stay");
    Check(Count(out, "[Menus]") == 1 && Count(out, "[Race]") == 1, "1.1.1 file: no second section");
    Check(out.find("InviteKey=A\r\n; The button pictures in the online menus") != std::string::npos &&
          out.find("PadPictures=auto\r\n\r\n[Race]") != std::string::npos, "1.1.1 file: PadPictures and its comments at the end of [Menus]");
    {   // removing what was added gives the player's file back, byte for byte
        std::string back = out;
        size_t a = back.find("; The button pictures"), b = back.find("PadPictures=auto\r\n") + 18;
        back.erase(a, b - a);
        Check(back == player, "1.1.1 file: nothing else changed");
    }
    std::string again;
    Check(!config_merge::ConfigMerge(tmpl, out, again), "merging the merged file again adds nothing");

    // LF file, a whole section missing, no newline at the end.
    std::string noUpdate = old.substr(0, old.find("[Update]"));
    while (!noUpdate.empty() && (noUpdate.back() == '\n' || noUpdate.back() == '\r')) noUpdate.pop_back();
    Check(config_merge::ConfigMerge(tmpl, noUpdate, out, &added) && added.size() == 2, "no [Update], no final newline: 2 options added");
    Check(Count(out, "[Update]") == 1 && out.find("Muted=\n\n[Update]\n") != std::string::npos && out.back() == '\n' &&
          out.find('\r') == std::string::npos, "no [Update]: the section goes at the end after one blank line, LF kept");
    Check(AllOptionsReadable(out), "no [Update]: Windows reads every option");

    // The last section lacks a key and the file has no final newline.
    std::string tailless = Replace(ToCrlf(tmpl), "\r\n; 1 = at game start", "\r\n; x");
    tailless = tailless.substr(0, tailless.find("\r\nAuto=1"));  // [Update] with comments only, no newline at the end
    Check(config_merge::ConfigMerge(tmpl, tailless, out, &added) && added.size() == 1 && added[0] == "[Update] Auto", "last section, no final newline: Auto added");
    Check(NoBareLf(out) && out.size() >= 6 && out.compare(out.size() - 6, 6, "Auto=1") == 0 && WinRead(out, "Update", "Auto") == "1",
          "last section, no final newline: added after a CRLF, still no final newline");

    // Case and spacing: present keys are found the way Windows finds them.
    std::string cased = Replace(Replace(tmpl, "PadPictures=auto", "  padpictures = xbox"), "[Race]", "[RACE]");
    Check(!config_merge::ConfigMerge(tmpl, cased, out), "case and spaces: nothing added");

    // Spaces inside the brackets: Windows trims them too ("[ Menus ]" is Menus), so the section is found.
    std::string spaced = Replace(Replace(tmpl, "[Menus]", "[ Menus ]"), "\nPadPictures=auto", "");
    Check(WinRead(spaced, "Menus", "InviteKey") == "A", "spaces in brackets: Windows reads [ Menus ] as Menus");
    Check(config_merge::ConfigMerge(tmpl, spaced, out, &added) && added.size() == 1 && Count(out, "Menus ]") == 1 && Count(out, "[Menus]") == 0 &&
          WinRead(out, "Menus", "PadPictures") == "auto", "spaces in brackets: the key goes into that section");

    // Mixed line ends: the added lines take the most common one.
    std::string mixed = Replace(old, "[Race]\n", "[Race]\r\n");
    Check(config_merge::ConfigMerge(tmpl, mixed, out) && Count(out, "\r\n") == 1, "mostly LF with one CRLF: added lines use LF");

    // A commented-out key counts as missing (Windows doesn't read it either); the comment stays.
    std::string commented = Replace(tmpl, "PadPictures=auto", ";PadPictures=xbox");
    Check(config_merge::ConfigMerge(tmpl, commented, out, &added) && added.size() == 1 && out.find(";PadPictures=xbox") != std::string::npos &&
          WinRead(out, "Menus", "PadPictures") == "auto", "commented-out key: added, the comment kept");

    // A second [Menus] further down (Windows reads only the first): the key goes into the first one.
    std::string twice = old + "\n[Menus]\nPadPictures=playstation\n";
    Check(config_merge::ConfigMerge(tmpl, twice, out, &added) && Count(out, "[Menus]") == 2 &&
          out.find("PadPictures=auto\n\n[Race]") != std::string::npos, "second [Menus]: added to the first one");

    // An empty file gets everything; so does a file with only [Debug].
    Check(config_merge::ConfigMerge(tmpl, "", out, &added) && added.size() == opts.size() && AllOptionsReadable(out), "empty file: every option added");
    Check(config_merge::ConfigMerge(tmpl, "[Debug]\r\nTestPlayers=3\r\n", out, &added) && added.size() == opts.size() &&
          AllOptionsReadable(out) && WinRead(out, "Debug", "TestPlayers") == "3" && NoBareLf(out), "only [Debug]: every option added, CRLF");

    // A UTF-8 BOM stays; UTF-16, NUL bytes and huge files are left alone.
    Check(config_merge::ConfigMerge(tmpl, "\xEF\xBB\xBF" + old, out) && out.compare(0, 3, "\xEF\xBB\xBF") == 0 && Count(out, "[Menus]") == 1,
          "UTF-8 BOM: kept, sections still found");
    Check(!config_merge::ConfigMerge(tmpl, std::string("\xFF\xFE[\0M\0", 6), out) && out.empty(), "UTF-16: left alone");
    Check(!config_merge::ConfigMerge(tmpl, std::string("[Menus]\n\0x=1\n", 13), out), "NUL bytes: left alone");
    Check(!config_merge::ConfigMerge(tmpl, std::string((1 << 20) + 1, ';'), out), "over 1 MB: left alone");

    printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
