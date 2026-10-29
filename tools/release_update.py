"""The auto-update's release files (notes/AUTO-UPDATE.md): from out/release/SteamMultiplayer-<version>.zip (tools/release_zip.py)
writes out/release/update.txt and signs it (update.txt.sig, tools/release_sign.py, the current key). The GitHub release
v<version> then carries three assets: the zip, update.txt, update.txt.sig. Refuses a developer version, a compressed zip,
a zip with anything outside "Steam Multiplayer/", and a key that isn't the one built into the DLL (src/update_keys.h).
Then run build/test_newversion.exe --check out/release (the DLL's own checks on the real files).

    python -I tools/release_update.py <repo root> [--mod <folder> --zip <zip> --out <folder>] [--test-key <private blob>]

(--mod/--zip/--out: a test release elsewhere, e.g. the end-to-end test's; the defaults are the real release's. --test-key: sign
with a test key made at test time instead of the real current key; only a test build of the DLL accepts it.)
"""
import datetime
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import release_sign  # noqa: E402

ROOT_DIR = "Steam Multiplayer/"


def sha(data: bytes) -> str:
    """SHA-256 hex."""
    return hashlib.sha256(data).hexdigest()


def built_in_keys(root: Path) -> list[bytes]:
    """The two public keys (X||Y) compiled into the DLL."""
    text = (root / "src" / "update_keys.h").read_text(encoding="ascii")
    values = [int(v, 16) for v in re.findall(r"0x([0-9a-f]{2})", text)]
    assert len(values) == 128, "src/update_keys.h doesn't hold two 64-byte keys"
    return [bytes(values[:64]), bytes(values[64:])]


def main() -> None:
    """Write and sign update.txt."""
    root = Path(sys.argv[1]).resolve()
    opt = {k: Path(sys.argv[sys.argv.index(k) + 1]).resolve() for k in ("--mod", "--zip", "--out", "--test-key") if k in sys.argv}
    mod = opt.get("--mod", root / "out" / "Steam Multiplayer")
    version = re.search(r"^version=(.+)$", (mod / "mod.ini").read_text(encoding="ascii"), re.M).group(1).strip()
    if not re.fullmatch(r"\d{1,6}(\.\d{1,6}){0,3}", version):
        raise SystemExit(f"REFUSED: {version} is not a release version (developer builds never go out as updates)")
    out = opt.get("--out", root / "out" / "release")
    zpath = opt.get("--zip", out / f"SteamMultiplayer-{version}.zip")
    data = zpath.read_bytes()
    files = []
    with zipfile.ZipFile(zpath) as z:
        for info in z.infolist():
            if info.compress_type != zipfile.ZIP_STORED:
                raise SystemExit(f"REFUSED: {info.filename} is compressed (rebuild with tools/release_zip.py)")
            if not info.filename.startswith(ROOT_DIR):
                raise SystemExit(f"REFUSED: {info.filename} is outside {ROOT_DIR}")
            rel = info.filename[len(ROOT_DIR):]
            if not rel or rel.endswith("/"):
                continue
            body = z.read(info)
            files.append((rel, body))
            assert body == (mod / rel).read_bytes(), f"{rel} differs from out/Steam Multiplayer"
        desc = json.loads(z.read(ROOT_DIR + "allstars-mod.json"))
    caps = desc.get("loader", {}).get("capabilities", [])
    assert all(re.fullmatch(r"[a-z0-9_-]{1,64}", c) for c in caps), caps
    lines = ["format=1", f"version={version}", f"date={datetime.date.today().isoformat()}", "loader_caps=" + ",".join(caps),
             f"zip={zpath.name} {len(data)} {sha(data)}"]
    lines += [f"file={sha(body)} {len(body)} {rel}" for rel, body in sorted(files)]
    text = ("\n".join(lines) + "\n").encode("ascii")
    manifest = out / "update.txt"
    manifest.write_bytes(text)
    if "--test-key" in opt:
        key = opt["--test-key"].read_bytes()
        if release_sign.public_of(key)[8:] in built_in_keys(root):
            raise SystemExit("REFUSED: --test-key is a real release key")
    else:
        key = release_sign.private_key("current")
        if release_sign.public_of(key)[8:] != built_in_keys(root)[0]:
            raise SystemExit("REFUSED: the current signing key isn't the one built into the DLL (src/update_keys.h)")
    sig = release_sign.sign(key, text)
    assert release_sign.verify(release_sign.public_of(key), text, sig)
    (out / "update.txt.sig").write_bytes(sig)
    which = "a TEST key" if "--test-key" in opt else "the current key"
    print(f"{manifest}: version {version}, {len(files)} files, zip {zpath.name} ({len(data)} bytes), signed with {which}")


if __name__ == "__main__":
    main()
