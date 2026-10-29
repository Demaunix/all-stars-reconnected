"""Release signing for the auto-update (notes/AUTO-UPDATE.md): ECDSA P-256 over SHA-256 through Windows CNG (bcrypt.dll), the
same algorithm the mod's DLL verifies with BCryptVerifySignature. No third-party code.

    python -I tools/release_sign.py keys              make the two key pairs (current, spare); refuses if they exist
    python -I tools/release_sign.py public            print both public keys as the C array src/update_keys.h holds
    python -I tools/release_sign.py sign <file>       write <file>.sig (64 bytes r||s) with the current key, then verify it
    python -I tools/release_sign.py verify <file>     check <file>.sig against either public key

The private keys live in %USERPROFILE%\\.asr-release-keys\\ (never in a repository); keep a copy of that folder somewhere safe.
"""
import ctypes
import hashlib
import os
import sys
from ctypes import wintypes
from pathlib import Path

KEY_DIR = Path(os.environ["USERPROFILE"]) / ".asr-release-keys"
NAMES = ("current", "spare")
ALG = "ECDSA_P256"
PRIVATE_BLOB, PUBLIC_BLOB = "ECCPRIVATEBLOB", "ECCPUBLICBLOB"
MAGIC_PUBLIC, MAGIC_PRIVATE = 0x31534345, 0x32534345  # ECS1, ECS2 (BCRYPT_ECDSA_PUBLIC/PRIVATE_P256_MAGIC)

bcrypt = ctypes.WinDLL("bcrypt.dll")
HANDLE = ctypes.c_void_p
for name, args in {
    "BCryptOpenAlgorithmProvider": [ctypes.POINTER(HANDLE), wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.ULONG],
    "BCryptCloseAlgorithmProvider": [HANDLE, wintypes.ULONG],
    "BCryptGenerateKeyPair": [HANDLE, ctypes.POINTER(HANDLE), wintypes.ULONG, wintypes.ULONG],
    "BCryptFinalizeKeyPair": [HANDLE, wintypes.ULONG],
    "BCryptExportKey": [HANDLE, HANDLE, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG],
    "BCryptImportKeyPair": [HANDLE, HANDLE, wintypes.LPCWSTR, ctypes.POINTER(HANDLE), ctypes.c_void_p, wintypes.ULONG, wintypes.ULONG],
    "BCryptSignHash": [HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG],
    "BCryptVerifySignature": [HANDLE, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, wintypes.ULONG],
    "BCryptDestroyKey": [HANDLE],
}.items():
    getattr(bcrypt, name).argtypes = args
    getattr(bcrypt, name).restype = ctypes.c_long  # NTSTATUS


def ok(status: int, what: str) -> None:
    """Stop on a failed CNG call."""
    if status != 0:
        raise SystemExit(f"{what} failed: NTSTATUS 0x{status & 0xffffffff:08x}")


def provider() -> HANDLE:
    """An ECDSA P-256 algorithm handle."""
    h = HANDLE()
    ok(bcrypt.BCryptOpenAlgorithmProvider(ctypes.byref(h), ALG, None, 0), "BCryptOpenAlgorithmProvider")
    return h


def export(key: HANDLE, blob: str) -> bytes:
    """A key as a CNG blob."""
    n = wintypes.ULONG()
    ok(bcrypt.BCryptExportKey(key, None, blob, None, 0, ctypes.byref(n), 0), "BCryptExportKey (size)")
    buf = ctypes.create_string_buffer(n.value)
    ok(bcrypt.BCryptExportKey(key, None, blob, buf, n, ctypes.byref(n), 0), "BCryptExportKey")
    return buf.raw[:n.value]


def load(blob: bytes, kind: str) -> tuple[HANDLE, HANDLE]:
    """(algorithm, key) handles for a stored blob."""
    h, key = provider(), HANDLE()
    buf = ctypes.create_string_buffer(blob, len(blob))
    ok(bcrypt.BCryptImportKeyPair(h, None, kind, ctypes.byref(key), buf, len(blob), 0), "BCryptImportKeyPair")
    return h, key


def close(h: HANDLE, key: HANDLE) -> None:
    """Free CNG handles."""
    bcrypt.BCryptDestroyKey(key)
    bcrypt.BCryptCloseAlgorithmProvider(h, 0)


def public_of(private: bytes) -> bytes:
    """The 72-byte public blob (header + X + Y) of a private blob."""
    h, key = load(private, PRIVATE_BLOB)
    try:
        pub = export(key, PUBLIC_BLOB)
    finally:
        close(h, key)
    assert len(pub) == 72 and int.from_bytes(pub[:4], "little") == MAGIC_PUBLIC and int.from_bytes(pub[4:8], "little") == 32
    return pub


def sign(private: bytes, data: bytes) -> bytes:
    """ECDSA P-256 signature (r||s, 64 bytes) of SHA-256(data)."""
    digest = hashlib.sha256(data).digest()
    h, key = load(private, PRIVATE_BLOB)
    try:
        sig, n = ctypes.create_string_buffer(64), wintypes.ULONG()
        ok(bcrypt.BCryptSignHash(key, None, digest, 32, sig, 64, ctypes.byref(n), 0), "BCryptSignHash")
        assert n.value == 64
        return sig.raw
    finally:
        close(h, key)


def verify(public: bytes, data: bytes, sig: bytes) -> bool:
    """True when sig is a valid signature of data under this public blob."""
    if len(sig) != 64:
        return False
    digest = hashlib.sha256(data).digest()
    h, key = load(public, PUBLIC_BLOB)
    try:
        return bcrypt.BCryptVerifySignature(key, None, digest, 32, sig, 64, 0) == 0
    finally:
        close(h, key)


def private_key(name: str) -> bytes:
    """A stored private blob (checked)."""
    blob = (KEY_DIR / f"{name}.key").read_bytes()
    if len(blob) != 104 or int.from_bytes(blob[:4], "little") != MAGIC_PRIVATE:
        raise SystemExit(f"{KEY_DIR / (name + '.key')} is not an ECDSA P-256 private key")
    return blob


def new_private() -> bytes:
    """A fresh ECDSA P-256 private blob (104 bytes), kept nowhere (tests use it as their own key)."""
    h, key = provider(), HANDLE()
    ok(bcrypt.BCryptGenerateKeyPair(h, ctypes.byref(key), 256, 0), "BCryptGenerateKeyPair")
    ok(bcrypt.BCryptFinalizeKeyPair(key, 0), "BCryptFinalizeKeyPair")
    blob = export(key, PRIVATE_BLOB)
    close(h, key)
    assert len(blob) == 104
    return blob


def make_keys() -> None:
    """Create both key pairs; never overwrite."""
    if any((KEY_DIR / f"{n}.key").exists() for n in NAMES):
        raise SystemExit(f"REFUSED: keys already exist in {KEY_DIR}")
    KEY_DIR.mkdir(exist_ok=True)
    for name in NAMES:
        blob = new_private()
        path = KEY_DIR / f"{name}.key"
        with open(path, "xb") as f:  # (x: never overwrite)
            f.write(blob)
        probe = b"asr release key self-test " + name.encode()
        assert verify(public_of(blob), probe, sign(blob, probe)), name
        print(f"made {path}")


def public_array() -> str:
    """Both public keys (X||Y, 64 bytes each) as the C source of src/update_keys.h."""
    lines = ["// The auto-update's public keys (tools/release_sign.py public): X||Y of ECDSA P-256, [0] current, [1] spare.",
             "#pragma once", "#include <stdint.h>", "", "static const uint8_t kUpdateKeys[2][64] = {"]
    for name in NAMES:
        xy = public_of(private_key(name))[8:]
        rows = [", ".join(f"0x{b:02x}" for b in xy[i:i + 16]) for i in range(0, 64, 16)]
        lines.append("    {  // " + name)
        lines += [f"        {r}," for r in rows]
        lines.append("    },")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main() -> None:
    """Command line."""
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "keys":
        make_keys()
    elif cmd == "public":
        sys.stdout.write(public_array())
    elif cmd == "sign":
        path = Path(sys.argv[2])
        data, key = path.read_bytes(), private_key("current")
        sig = sign(key, data)
        assert verify(public_of(key), data, sig)
        Path(str(path) + ".sig").write_bytes(sig)
        print(f"signed {path} ({len(data)} bytes, sha256 {hashlib.sha256(data).hexdigest()})")
    elif cmd == "verify":
        path = Path(sys.argv[2])
        data, sig = path.read_bytes(), Path(str(path) + ".sig").read_bytes()
        good = [n for n in NAMES if verify(public_of(private_key(n)), data, sig)]
        print(f"{path}: signed by {good[0]} key" if good else f"{path}: BAD signature")
        sys.exit(0 if good else 1)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
