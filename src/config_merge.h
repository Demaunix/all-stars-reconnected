// Adding the option lines a player's config.ini lacks (the auto-update keeps the player's config.ini, so options added
// by later versions, e.g. 1.2.0's [Menus] PadPictures, never showed up in it; the code defaults them, so only the
// line, and its comment, was missing).
//
// ConfigMerge(template, user): every [Section] / Key of the shipped config.ini (the template) that the player's file
// lacks is added, with the comment lines just above it in the template and the template's value (which is the code's
// default, so nothing plays differently). A missing key goes at the end of its section's FIRST occurrence (the one
// GetPrivateProfile* reads; never a second [Section] of the same name); a missing section goes at the end of the file.
// Nothing else changes: the player's values, comments, extra sections ([Debug], [Friends]), order and line ends
// (CRLF or LF, as the file has them) stay; only blank lines at the very end become one when a section is added. Keys and sections compare without case, as Windows does. A key counts as
// present only as a real "Key=" line inside its section (a commented-out one doesn't). Keys before the first section
// header are ignored. Returns false (and leaves `out` empty) when nothing is missing or the file can't be merged safely
// (UTF-16, NUL bytes, over 1 MB).
#pragma once
#include <string>
#include <vector>

namespace config_merge {

inline std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

inline std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

// "[Name]" -> "name" (lowercase); "" when the line isn't a section header.
inline std::string SectionOf(const std::string& line) {
    std::string t = Trim(line);
    if (t.size() < 2 || t[0] != '[') return "";
    size_t close = t.find(']');
    if (close == std::string::npos) return "";
    std::string name = Lower(Trim(t.substr(1, close - 1)));
    return name.empty() ? std::string("\x01") : name;  // "[]" is a header too (it ends the section above)
}

// "Key=value" -> "key" (lowercase); "" for comments, blank lines and lines without '='.
inline std::string KeyOf(const std::string& line) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return "";
    size_t eq = t.find('=');
    if (eq == std::string::npos || eq == 0) return "";
    return Lower(Trim(t.substr(0, eq)));
}

inline std::vector<std::string> SplitLines(const std::string& text) {  // on '\n'; each line keeps its '\r'
    std::vector<std::string> lines;
    size_t start = 0;
    for (;;) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) { lines.push_back(text.substr(start)); break; }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;  // "a\nb\n" -> {"a", "b", ""}: joining with '\n' gives the text back
}

struct Block { std::string key, name; std::vector<std::string> lines; };  // key lowercase, name as written; its comment lines + its own line (no '\r')
struct Section { std::string name, header; std::vector<Block> blocks; };

inline std::vector<Section> ParseTemplate(const std::string& tmpl) {
    std::vector<Section> out;
    std::vector<std::string> pending;  // comment lines since the last key or header
    for (auto& raw : SplitLines(tmpl)) {
        std::string line = raw;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string sec = SectionOf(line), key = KeyOf(line);
        if (!sec.empty()) { out.push_back(Section{sec, Trim(line), {}}); pending.clear(); }
        else if (!key.empty()) {
            if (!out.empty()) {
                std::string t = Trim(line);
                pending.push_back(line);
                out.back().blocks.push_back(Block{key, Trim(t.substr(0, t.find('='))), pending});
            }
            pending.clear();
        } else if (Trim(line).empty()) pending.clear();
        else pending.push_back(line);
    }
    return out;
}

// `added` gets "Section/Key" for each added option (for the log).
inline bool ConfigMerge(const std::string& tmpl, const std::string& user, std::string& out, std::vector<std::string>* added = nullptr) {
    out.clear();
    if (user.size() > (1u << 20) || user.find('\0') != std::string::npos) return false;
    if (user.size() >= 2 && ((unsigned char)user[0] == 0xFF && (unsigned char)user[1] == 0xFE)) return false;  // UTF-16
    if (user.size() >= 2 && ((unsigned char)user[0] == 0xFE && (unsigned char)user[1] == 0xFF)) return false;
    std::string bom = user.compare(0, 3, "\xEF\xBB\xBF") == 0 ? std::string("\xEF\xBB\xBF") : std::string();
    std::vector<std::string> lines = SplitLines(user.substr(bom.size()));
    size_t crlf = 0, lf = 0;  // the added lines take the file's usual line end (CRLF when most lines have it)
    for (size_t i = 0; i < user.size(); ++i) if (user[i] == '\n') { ++lf; if (i && user[i - 1] == '\r') ++crlf; }
    const std::string cr = crlf * 2 > lf ? "\r" : "";

    std::vector<Section> want = ParseTemplate(tmpl);
    std::vector<std::string> names;  // added, for the log
    // Sections are handled one at a time, re-scanning the lines each time (they move as lines go in; the file is small).
    for (auto& s : want) {
        size_t header = std::string::npos, end = lines.size();
        for (size_t i = 0; i < lines.size(); ++i) {
            std::string sec = SectionOf(lines[i]);
            if (sec.empty()) continue;
            if (header == std::string::npos) { if (sec == s.name) header = i; }
            else { end = i; break; }
        }
        std::vector<const Block*> missing;
        if (header != std::string::npos) {
            for (auto& b : s.blocks) {
                bool have = false;
                for (size_t i = header + 1; i < end && !have; ++i) have = KeyOf(lines[i]) == b.key;
                if (!have) missing.push_back(&b);
            }
        } else {
            for (auto& b : s.blocks) missing.push_back(&b);
        }
        if (missing.empty()) continue;
        std::vector<std::string> insert;
        if (header == std::string::npos) {
            // A new section at the end, after one blank line; the file then ends with a newline.
            while (!lines.empty() && Trim(lines.back()).empty()) lines.pop_back();
            if (!lines.empty() && !cr.empty() && lines.back().back() != '\r') lines.back() += cr;  // (it had no newline)
            if (!lines.empty()) insert.push_back(cr);
            insert.push_back(s.header + cr);
        }
        for (auto* b : missing) {
            for (auto& l : b->lines) insert.push_back(l + cr);
            names.push_back(s.header + " " + b->name);
        }
        if (header == std::string::npos) {
            insert.push_back("");  // the final newline
            lines.insert(lines.end(), insert.begin(), insert.end());
        } else {
            size_t at = header + 1;  // after the section's last non-blank line
            for (size_t i = header + 1; i < end; ++i) if (!Trim(lines[i]).empty()) at = i + 1;
            if (at == lines.size()) {
                // The section is the file's last and its last line has no newline: give it one, and keep none after ours.
                lines.back() += cr;
                insert.back() = insert.back().substr(0, insert.back().size() - cr.size());
            }
            lines.insert(lines.begin() + (ptrdiff_t)at, insert.begin(), insert.end());
        }
    }
    if (names.empty()) return false;
    out = bom;
    for (size_t i = 0; i < lines.size(); ++i) { if (i) out += '\n'; out += lines[i]; }
    if (added) *added = names;
    return true;
}

}  // namespace config_merge
