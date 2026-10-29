// The auto-update's checks and file work (notes/AUTO-UPDATE.md): the signed manifest update.txt, version and path rules,
// the STORED release zip, staging and the ready marker for ChaoGarage's swap at the next start. No network here (update.cpp
// downloads); header-only so tests/test_update.cpp can run every refusal and the staging on a fake game folder.
// Every function fails closed: on any doubt it returns a reason and changes nothing it can't undo.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <stdint.h>
#include <string.h>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "update_keys.h"

struct UpdateFile {
    std::string path;  // relative to the mod folder, '/' separators
    uint64_t size = 0;
    uint8_t sha[32] = {};
};

struct UpdateManifest {
    int format = 0;
    std::string version, date, zipName;
    uint64_t zipSize = 0;
    uint8_t zipSha[32] = {};
    std::vector<std::string> loaderCaps;  // every one must be in the running loader's list
    std::vector<UpdateFile> files;
};

const uint64_t kUpdateMaxManifest = 64 * 1024;
const uint64_t kUpdateMaxZip = 200ull * 1024 * 1024;
const size_t kUpdateMaxFiles = 1000;
const char kUpdateZipRoot[] = "Steam Multiplayer/";
// The ChaoGarage capability (1.3.0) that puts a staged update in place at start (notes/AUTO-UPDATE.md "The handshake").
const char kUpdateLoaderCapability[] = "pending-mod-updates";

// ---------------------------------------------------------------- hashing and signatures
class UpdateSha256 {
public:
    UpdateSha256() {
        ok_ = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg_, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg_, &h_, nullptr, 0, nullptr, 0, 0));
    }
    ~UpdateSha256() {
        if (h_) BCryptDestroyHash(h_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    void Add(const void* p, size_t n) { ok_ = ok_ && BCRYPT_SUCCESS(BCryptHashData(h_, (PUCHAR)p, (ULONG)n, 0)); }
    bool Finish(uint8_t out[32]) { return ok_ && BCRYPT_SUCCESS(BCryptFinishHash(h_, out, 32, 0)); }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE h_ = nullptr;
    bool ok_ = false;
};

inline bool UpdateHash(const void* p, size_t n, uint8_t out[32]) {
    UpdateSha256 s;
    s.Add(p, n);
    return s.Finish(out);
}

// ECDSA P-256 over SHA-256 (tools/release_sign.py): the index of the key that signed `text`, or -1.
inline int UpdateSignedBy(const std::string& text, const uint8_t* sig, size_t sigLen, const uint8_t (*keys)[64] = kUpdateKeys,
                          int keyCount = 2) {
    uint8_t digest[32];
    if (sigLen != 64 || !UpdateHash(text.data(), text.size(), digest)) return -1;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) return -1;
    int found = -1;
    for (int k = 0; k < keyCount && found < 0; ++k) {
        uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
        BCRYPT_ECCKEY_BLOB* head = (BCRYPT_ECCKEY_BLOB*)blob;
        head->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        head->cbKey = 32;
        memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), keys[k], 64);
        BCRYPT_KEY_HANDLE key = nullptr;
        if (BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, blob, sizeof(blob), 0))) {
            if (BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, digest, 32, (PUCHAR)sig, 64, 0))) found = k;
            BCryptDestroyKey(key);
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return found;
}

// ---------------------------------------------------------------- versions and names
// A release version is 1-4 dot-separated numbers ("1.1.1"); anything else (a developer build "1.1.1-dp9") is not.
inline bool UpdateVersionParts(const std::string& v, std::vector<uint32_t>& parts) {
    parts.clear();
    size_t i = 0;
    while (i < v.size()) {
        uint64_t n = 0;
        size_t start = i;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9' && i - start < 6) n = n * 10 + (v[i++] - '0');
        if (i == start || (i < v.size() && v[i] != '.')) return false;
        parts.push_back((uint32_t)n);
        if (i < v.size() && ++i == v.size()) return false;  // a trailing dot
    }
    return !parts.empty() && parts.size() <= 4;
}

inline bool UpdateIsRelease(const std::string& v) {
    std::vector<uint32_t> p;
    return UpdateVersionParts(v, p);
}

// True when `candidate` is a release newer than the release `running` (missing parts count as 0).
inline bool UpdateNewer(const std::string& candidate, const std::string& running) {
    std::vector<uint32_t> a, b;
    if (!UpdateVersionParts(candidate, a) || !UpdateVersionParts(running, b)) return false;
    for (size_t i = 0; i < 4; ++i) {
        uint32_t x = i < a.size() ? a[i] : 0, y = i < b.size() ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

// A file path inside the mod folder: relative, '/'-separated, plain ASCII names, no "." / ".." parts, no drive, stream or
// device tricks. The loader's own files (the folder holds no others) never need more.
inline bool UpdateSafePath(const std::string& p) {
    if (p.empty() || p.size() > 200 || p.front() == '/' || p.back() == '/') return false;
    size_t part = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            std::string name = p.substr(part, i - part);
            if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ' || name.front() == ' ')
                return false;
            std::string stem = name.substr(0, name.find('.'));
            for (auto& c : stem) c = (char)toupper((unsigned char)c);
            for (const char* dev : {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"})
                if (stem == dev) return false;
            part = i + 1;
            continue;
        }
        char c = p[i];
        bool okChar = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr(" ._-+()&!,'", c);
        if (!okChar) return false;
    }
    return true;
}

inline bool UpdateHex32(const std::string& s, uint8_t out[32]) {
    if (s.size() != 64) return false;
    for (int i = 0; i < 32; ++i) {
        int v = 0;
        for (int k = 0; k < 2; ++k) {
            char c = s[i * 2 + k];
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;  // lowercase only
            if (d < 0) return false;
            v = v * 16 + d;
        }
        out[i] = (uint8_t)v;
    }
    return true;
}

inline bool UpdateNumber(const std::string& s, uint64_t& out) {
    if (s.empty() || s.size() > 12) return false;
    out = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        out = out * 10 + (c - '0');
    }
    return true;
}

// ---------------------------------------------------------------- the manifest
// update.txt: "key=value" lines, LF or CRLF. format=1, version=, date=, loader_caps=a,b (may be empty), zip=<name> <size>
// <sha256>, file=<sha256> <size> <path> (one per file in the mod folder). Unknown keys are refused (a newer format must
// say format=2, which this build refuses).
inline const char* UpdateParse(const std::string& text, UpdateManifest& m) {
    m = UpdateManifest();
    if (text.size() > kUpdateMaxManifest) return "the manifest is too big";
    bool haveZip = false, haveDate = false, haveCaps = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? text.size() : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        for (char c : line)
            if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return "the manifest has a control or non-ASCII character";
        size_t eq = line.find('=');
        if (eq == std::string::npos) return "a manifest line has no '='";
        std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "format") {
            if (m.format || value != "1") return "the manifest's format isn't 1";
            m.format = 1;
        } else if (!m.format) {
            return "the manifest doesn't start with format=1";
        } else if (key == "version") {
            if (!m.version.empty() || !UpdateIsRelease(value)) return "the manifest's version isn't a release version";
            m.version = value;
        } else if (key == "date") {
            if (haveDate || value.size() != 10) return "the manifest's date is not YYYY-MM-DD";
            m.date = value;
            haveDate = true;
        } else if (key == "loader_caps") {
            if (haveCaps) return "loader_caps twice";
            haveCaps = true;
            size_t p = 0;
            while (p < value.size()) {
                size_t c = value.find(',', p);
                std::string cap = value.substr(p, c == std::string::npos ? std::string::npos : c - p);
                p = c == std::string::npos ? value.size() : c + 1;
                if (cap.empty() || cap.size() > 64) return "a loader capability is empty or too long";
                for (char ch : cap)
                    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return "a loader capability isn't a lowercase name";
                m.loaderCaps.push_back(cap);
            }
        } else if (key == "zip") {
            size_t s2 = value.rfind(' '), s1 = s2 == std::string::npos ? std::string::npos : value.rfind(' ', s2 - 1);
            if (haveZip || s1 == std::string::npos || s1 == 0) return "the manifest's zip line is malformed";
            m.zipName = value.substr(0, s1);
            if (!UpdateNumber(value.substr(s1 + 1, s2 - s1 - 1), m.zipSize) || !UpdateHex32(value.substr(s2 + 1), m.zipSha))
                return "the manifest's zip line is malformed";
            if (!UpdateSafePath(m.zipName) || m.zipName.find('/') != std::string::npos || m.zipName.size() < 5 ||
                _stricmp(m.zipName.c_str() + m.zipName.size() - 4, ".zip") != 0)
                return "the manifest's zip name isn't a plain .zip name";
            if (m.zipSize == 0 || m.zipSize > kUpdateMaxZip) return "the manifest's zip size is out of range";
            haveZip = true;
        } else if (key == "file") {
            if (m.files.size() >= kUpdateMaxFiles) return "the manifest lists too many files";
            UpdateFile f;
            if (value.size() < 68 || value[64] != ' ' || !UpdateHex32(value.substr(0, 64), f.sha)) return "a file line is malformed";
            size_t sp = value.find(' ', 65);
            if (sp == std::string::npos || !UpdateNumber(value.substr(65, sp - 65), f.size)) return "a file line is malformed";
            f.path = value.substr(sp + 1);
            if (!UpdateSafePath(f.path)) return "a file path leaves the mod folder or has unusual characters";
            for (auto& g : m.files)
                if (_stricmp(g.path.c_str(), f.path.c_str()) == 0) return "a file is listed twice";
            m.files.push_back(f);
        } else {
            return "the manifest has an unknown key";
        }
    }
    if (!m.format || m.version.empty() || !haveDate || !haveCaps || !haveZip) return "the manifest is missing a line";
    bool modIni = false, dll = false;
    for (auto& f : m.files) {
        modIni = modIni || f.path == "mod.ini";
        dll = dll || f.path == "runtime/SteamMultiplayer.dll";
    }
    if (!modIni || !dll) return "the manifest lacks mod.ini or the runtime DLL";
    return nullptr;
}

// ---------------------------------------------------------------- the STORED zip
// Reads the central directory through `read(offset, buffer, count)` (false = short read). Only method 0 (stored), no
// encryption, no ZIP64, every name under "Steam Multiplayer/" (directories skipped). `entries` get the data offsets.
struct UpdateZipEntry {
    std::string path;  // without the "Steam Multiplayer/" root
    uint64_t offset = 0, size = 0;
};

using UpdateReader = std::function<bool(uint64_t, void*, size_t)>;

inline uint32_t UpdateU32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
inline uint16_t UpdateU16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }

inline const char* UpdateReadZip(uint64_t zipSize, const UpdateReader& read, std::vector<UpdateZipEntry>& entries) {
    entries.clear();
    if (zipSize < 22 || zipSize > kUpdateMaxZip) return "the zip's size is out of range";
    size_t tail = (size_t)(zipSize < 22 + 65535 ? zipSize : 22 + 65535);
    std::vector<uint8_t> t(tail);
    if (!read(zipSize - tail, t.data(), tail)) return "the zip can't be read";
    size_t eocd = SIZE_MAX;
    for (size_t i = tail - 22 + 1; i-- > 0;)
        if (UpdateU32(&t[i]) == 0x06054b50 && i + 22 + UpdateU16(&t[i + 20]) == tail) {
            eocd = i;
            break;
        }
    if (eocd == SIZE_MAX) return "the zip has no end record";
    const uint8_t* e = &t[eocd];
    uint32_t count = UpdateU16(e + 10), cdSize = UpdateU32(e + 12), cdOff = UpdateU32(e + 16);
    if (UpdateU16(e + 4) || UpdateU16(e + 6) || UpdateU16(e + 8) != count) return "the zip spans several disks";
    if (count == 0xffff || cdSize == 0xffffffff || cdOff == 0xffffffff) return "the zip uses ZIP64";
    if (count > kUpdateMaxFiles + 64 || (uint64_t)cdOff + cdSize > zipSize - tail + eocd) return "the zip's directory is out of range";
    std::vector<uint8_t> cd(cdSize);
    if (cdSize && !read(cdOff, cd.data(), cdSize)) return "the zip's directory can't be read";
    size_t p = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > cd.size() || UpdateU32(&cd[p]) != 0x02014b50) return "the zip's directory is damaged";
        uint16_t flags = UpdateU16(&cd[p + 8]), method = UpdateU16(&cd[p + 10]);
        uint32_t csize = UpdateU32(&cd[p + 20]), usize = UpdateU32(&cd[p + 24]), local = UpdateU32(&cd[p + 42]);
        uint16_t nlen = UpdateU16(&cd[p + 28]), xlen = UpdateU16(&cd[p + 30]), clen = UpdateU16(&cd[p + 32]);
        if (p + 46 + nlen + xlen + clen > cd.size()) return "the zip's directory is damaged";
        std::string name((const char*)&cd[p + 46], nlen);
        p += 46 + nlen + xlen + clen;
        if (flags & 1) return "the zip is encrypted";
        if (method != 0 || csize != usize) return "the zip is compressed (release zips must be stored)";
        if (name.compare(0, sizeof(kUpdateZipRoot) - 1, kUpdateZipRoot) != 0) return "a zip entry is outside \"Steam Multiplayer/\"";
        std::string rel = name.substr(sizeof(kUpdateZipRoot) - 1);
        if (rel.empty() || rel.back() == '/') {
            if (usize) return "a zip directory entry has data";
            continue;
        }
        if (!UpdateSafePath(rel)) return "a zip entry's path leaves the mod folder or has unusual characters";
        uint8_t lh[30];
        if ((uint64_t)local + 30 > zipSize || !read(local, lh, 30) || UpdateU32(lh) != 0x04034b50) return "a zip entry's header is damaged";
        if (UpdateU16(lh + 8) != 0 || UpdateU16(lh + 26) != nlen) return "a zip entry's header disagrees with the directory";
        uint64_t data = (uint64_t)local + 30 + UpdateU16(lh + 26) + UpdateU16(lh + 28);
        if (data + usize > cdOff) return "a zip entry's data runs into the directory";
        for (auto& o : entries)
            if (_stricmp(o.path.c_str(), rel.c_str()) == 0) return "a zip entry appears twice";
        entries.push_back({rel, data, usize});
    }
    return nullptr;
}

// The zip's files must be exactly the manifest's (same names, same sizes); hashes are checked while copying.
inline const char* UpdateMatch(const UpdateManifest& m, const std::vector<UpdateZipEntry>& entries) {
    if (entries.size() != m.files.size()) return "the zip's files differ from the manifest's";
    for (auto& f : m.files) {
        const UpdateZipEntry* e = nullptr;
        for (auto& z : entries)
            if (z.path == f.path) e = &z;
        if (!e || e->size != f.size) return "the zip's files differ from the manifest's";
    }
    return nullptr;
}

inline bool UpdateHasCaps(const std::vector<std::string>& need, const char* const* have) {
    for (auto& c : need) {
        bool found = false;
        for (auto k = have; k && *k && !found; ++k) found = c == *k;
        if (!found) return false;
    }
    return true;
}

// ---------------------------------------------------------------- files
inline std::wstring UpdateWide(const std::string& s) {
    std::wstring w(s.begin(), s.end());  // (paths are checked ASCII)
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

inline bool UpdateMakeDirs(const std::wstring& dir) {
    if (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    size_t cut = dir.find_last_of(L'\\');
    return cut != std::wstring::npos && cut > 2 && UpdateMakeDirs(dir.substr(0, cut)) && CreateDirectoryW(dir.c_str(), nullptr);
}

// Deletes a folder tree (never follows links: reparse points are removed as entries, not entered).
inline bool UpdateRemoveTree(const std::wstring& dir) {
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_NORMAL);
        return (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(dir.c_str()) != 0 : DeleteFileW(dir.c_str()) != 0;
    }
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    bool ok = true;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            ok = UpdateRemoveTree(dir + L"\\" + fd.cFileName) && ok;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(dir.c_str()) != 0 && ok;
}

// Copies every zip entry into `stage` (a new folder), checking each file's SHA-256 against the manifest. `stop` (optional) is
// asked between pieces: true ends it ("stopped"; the half-made folder is removed by the next start's clean-up).
using UpdateStop = std::function<bool()>;

inline const char* UpdateStage(const UpdateManifest& m, const std::vector<UpdateZipEntry>& entries, const UpdateReader& read,
                               const std::wstring& stage, const UpdateStop& stop = nullptr) {
    if (!UpdateRemoveTree(stage) || !UpdateMakeDirs(stage)) return "the staging folder can't be made";
    std::vector<uint8_t> buf(1 << 16);
    for (auto& f : m.files) {
        const UpdateZipEntry* e = nullptr;
        for (auto& z : entries)
            if (z.path == f.path) e = &z;
        if (!e) return "the zip's files differ from the manifest's";
        std::wstring path = stage + L"\\" + UpdateWide(f.path);
        if (!UpdateMakeDirs(path.substr(0, path.find_last_of(L'\\')))) return "a staged folder can't be made";
        HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) return "a staged file can't be written";
        UpdateSha256 sha;
        bool ok = true, stopped = false;
        for (uint64_t done = 0; ok && done < e->size;) {
            if (stop && stop()) {
                stopped = true;
                break;
            }
            size_t n = (size_t)(e->size - done < buf.size() ? e->size - done : buf.size());
            DWORD wrote = 0;
            ok = read(e->offset + done, buf.data(), n) && WriteFile(out, buf.data(), (DWORD)n, &wrote, nullptr) && wrote == n;
            sha.Add(buf.data(), n);
            done += n;
        }
        ok = FlushFileBuffers(out) && ok;
        CloseHandle(out);
        uint8_t got[32];
        if (stopped) return "stopped";
        if (!ok) return "a staged file can't be written";
        if (!sha.Finish(got) || memcmp(got, f.sha, 32) != 0) return "a file's SHA-256 differs from the manifest's";
    }
    return nullptr;
}

inline std::string UpdateHexOf(const uint8_t d[32]) {
    static const char* x = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 32; ++i) s += x[d[i] >> 4], s += x[d[i] & 15];
    return s;
}

// Every file under `dir` (relative '/' paths), with sizes and SHA-256, read back from disk. Refuses links and odd names.
inline const char* UpdateListFolder(const std::wstring& dir, const std::string& prefix, std::vector<UpdateFile>& out,
                                    const UpdateStop& stop = nullptr) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return "a staged folder can't be read";
    const char* why = nullptr;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        std::string name;
        for (const wchar_t* c = fd.cFileName; *c && !why; ++c)
            if (*c > 0x7e) why = "a staged file has a non-ASCII name";
            else name += (char)*c;
        if (why) break;
        std::string rel = prefix.empty() ? name : prefix + "/" + name;
        if (!UpdateSafePath(rel)) why = "a staged file has an unusual name";
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) why = "a staged entry is a link";
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) why = UpdateListFolder(dir + L"\\" + fd.cFileName, rel, out, stop);
        else if (out.size() >= kUpdateMaxFiles) why = "too many staged files";
        else if (stop && stop()) why = "stopped";
        else {
            UpdateFile f;
            f.path = rel;
            HANDLE in = CreateFileW((dir + L"\\" + fd.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (in == INVALID_HANDLE_VALUE) {
                why = "a staged file can't be read back";
                break;
            }
            UpdateSha256 sha;
            std::vector<uint8_t> buf(1 << 16);
            DWORD got = 0;
            bool ok = true;
            while ((ok = ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr) != 0) && got) {
                sha.Add(buf.data(), got);
                f.size += got;
            }
            CloseHandle(in);
            if (!ok || !sha.Finish(f.sha)) why = "a staged file can't be read back";
            else out.push_back(f);
        }
    } while (!why && FindNextFileW(h, &fd));
    FindClose(h);
    return why;
}

// The update's folders, all on the game's drive: <game>\mods\Steam Multiplayer (live) and <game>\mods-updates\ (outside
// mods\, so the loader never reads a staged or previous copy as a mod). notes/AUTO-UPDATE.md "The handshake".
struct UpdatePaths {
    std::wstring live, root;
    std::wstring Staged() const { return root + L"\\staged\\Steam Multiplayer"; }
    std::wstring Ready() const { return root + L"\\staged\\Steam Multiplayer.ready"; }
};

inline bool UpdateExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The in-game "updated" notice: ChaoGarage put an update in place at this start when the copy it replaced
// (mods-updates\previous\Steam Multiplayer\mod.ini) names another version than the running one, and that notice wasn't
// shown for the running version yet (mods-updates\notice.txt holds the last version announced). Never through a link.
inline std::string UpdateIniVersion(const std::wstring& ini) {
    if (!UpdateExists(ini) || (GetFileAttributesW(ini.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    wchar_t v[64] = {};
    GetPrivateProfileStringW(L"mod", L"version", L"", v, 64, ini.c_str());
    std::string out;
    for (wchar_t* c = v; *c; ++c) out += *c >= 0x20 && *c < 0x7f ? (char)*c : '?';
    return out;
}

inline std::string UpdateNoticeShownFor(const std::wstring& root) {
    std::string s;
    HANDLE h = CreateFileW((root + L"\\notice.txt").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return s;
    char buf[64];
    DWORD n = 0;
    if (ReadFile(h, buf, sizeof(buf), &n, nullptr)) s.assign(buf, n);
    CloseHandle(h);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

inline bool UpdateJustApplied(const std::wstring& root, const std::string& running) {
    std::string before = UpdateIniVersion(root + L"\\previous\\Steam Multiplayer\\mod.ini");
    return !before.empty() && before != running && UpdateNoticeShownFor(root) != running;
}

inline bool UpdateNoticeMark(const std::wstring& root, const std::string& running) {
    std::wstring path = root + L"\\notice.txt";
    if (UpdateExists(path) && (GetFileAttributesW(path.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(h, running.data(), (DWORD)running.size(), &n, nullptr) && n == running.size();
    CloseHandle(h);
    return ok;
}

// Under the lock, before anything else: a download and a staging without its marker (cut short) go. A staging WITH its marker
// is a complete update waiting for ChaoGarage at the next start: kept (true = one is waiting).
inline bool UpdateCleanUp(const UpdatePaths& p) {
    UpdateRemoveTree(p.root + L"\\download");
    DeleteFileW((p.Ready() + L".tmp").c_str());
    // (a .swapping journal is ChaoGarage's: a swap it began and must finish or undo; never touched here)
    if (UpdateExists(p.Ready()) || UpdateExists(p.root + L"\\staged\\Steam Multiplayer.swapping")) return true;
    UpdateRemoveTree(p.Staged());
    return false;
}

// The marker: the staged folder's own listing, read back from disk, plus what it replaces (the installed mod.ini's SHA-256:
// ChaoGarage refuses the update if the installed folder changed or is gone) and the player's files it keeps (config.ini,
// copied from the installed folder at the swap, so later edits count). Written as .ready.tmp and renamed LAST, so a marker
// always means a complete staging. ChaoGarage checks the folder against it before the swap.
inline const char* UpdateWriteReady(const UpdatePaths& p, const std::string& version, const std::string& replaces,
                                    const std::vector<UpdateFile>& files) {
    // keep=config.ini only when the release has one (a kept file the marker doesn't list would fail a later check).
    bool hasConfig = std::any_of(files.begin(), files.end(), [](const UpdateFile& f) { return f.path == "config.ini"; });
    std::string text = "format=1\nfolder=Steam Multiplayer\nversion=" + version + "\nreplaces=" + replaces + "\n" +
                       (hasConfig ? "keep=config.ini\n" : "");
    for (auto& f : files) text += "file=" + UpdateHexOf(f.sha) + " " + std::to_string(f.size) + " " + f.path + "\n";
    std::wstring tmp = p.Ready() + L".tmp";
    HANDLE out = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return "the ready marker can't be written";
    DWORD wrote = 0;
    bool ok = WriteFile(out, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size() && FlushFileBuffers(out);
    CloseHandle(out);
    if (!ok || !MoveFileExW(tmp.c_str(), p.Ready().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return "the ready marker can't be written";
    }
    return nullptr;
}

// The installed mod.ini's SHA-256 (hex), or "" when it can't be read.
inline std::string UpdateFileSha(const std::wstring& path) {
    HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (in == INVALID_HANDLE_VALUE) return "";
    UpdateSha256 sha;
    std::vector<uint8_t> buf(1 << 16);
    DWORD got = 0;
    bool ok = true;
    while ((ok = ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr) != 0) && got) sha.Add(buf.data(), got);
    CloseHandle(in);
    uint8_t d[32];
    return ok && sha.Finish(d) ? UpdateHexOf(d) : "";
}

// Stage, re-check the staged folder against the manifest (exactly its files, as read back from disk), then the marker.
// Nothing in mods\ changes here; ChaoGarage puts the folder in place at the next start (and copies config.ini over then).
inline const char* UpdateStageForLoader(const UpdateManifest& m, const std::vector<UpdateZipEntry>& entries, const UpdateReader& read,
                                        const UpdatePaths& p, const UpdateStop& stop = nullptr) {
    std::string replaces = UpdateFileSha(p.live + L"\\mod.ini");
    if (replaces.empty()) return "the installed mod.ini can't be read";
    if (const char* why = UpdateMatch(m, entries)) return why;
    if (const char* why = UpdateStage(m, entries, read, p.Staged(), stop)) return why;
    std::vector<UpdateFile> staged;
    if (const char* why = UpdateListFolder(p.Staged(), "", staged, stop)) return why;
    if (staged.size() != m.files.size()) return "the staged folder holds files the manifest doesn't list";
    for (auto& f : m.files) {
        const UpdateFile* s = nullptr;
        for (auto& g : staged)
            if (g.path == f.path) s = &g;
        if (!s || s->size != f.size || memcmp(s->sha, f.sha, 32) != 0) return "the staged folder differs from the manifest (changed while staging?)";
    }
    return UpdateWriteReady(p, m.version, replaces, staged);
}

// src/update.cpp (the game only): the background check at start, and its stop at rollback.
void AutoUpdateStart(const std::wstring& modDir, const char* runningVersion);
void AutoUpdateStop();
// The in-game notices (the main menu shows them, once each): kUpdateNoticeReady (an update was downloaded in this session:
// restart to use it), kUpdateNoticeApplied (this start runs the version ChaoGarage just put in place). Pending() names the
// one to show (0 = none); Shown() is called once it is up.
const int kUpdateNoticeReady = 1, kUpdateNoticeApplied = 2;
int AutoUpdateNoticePending();
void AutoUpdateNoticeShown(int kind);
