r"""Keyboard / controller icons on the lobby bars (Majority Drift Parity, the user's request 2026-10-27; answers 2026-11-08: the lobby
bars only).

Each lobby bar's voice node (row node 4: 4e7d2ee9, e9cc98e1, ... ; re\m7-voice\FINDINGS.md section 3) shows the white speech
bubble 24064704 (54x46) while that member talks. Voice chat is off by default, so the node is free nearly always: src\natives.cpp
shows it with the member's device picture (STEAMMP_LOBBY_INPUT_KEYBOARD / _CONTROLLER, these sprites) and gives the bubble back
while the member talks. The pictures are drawn here (no keyboard or controller art exists in the lobby scenes; an addition), in
the bubble's style: a white body with a soft grey shade and a dark navy edge, 54x46. They live on one new uncompressed page
(A8R8G8B8 128x64) merged like tools\mod_icons.py's (Codex's merge_ptex). A lobby scene without them (another mod's version of
the scene) keeps the stock behaviour: the C++ asks 0x47f400, which answers 0 for art not in the scene.

    import lobby_input_icons; payload = lobby_input_icons.add_icons(payload)
    python tools\lobby_input_icons.py     a preview on a lobby bar's blue (build\lobby_input_icons_preview.png) and the hashes
Needs numpy and Pillow (run without -I, as footer_keys.py).
"""
import io
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "codexui"))
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
from build_ui import check_relos, merge_ptex, pack, path_hash, ptex_info  # noqa: E402
from decode_ui_texture import sections  # noqa: E402
from probe import chunks  # noqa: E402

BUBBLE = 0x24064704  # the voice node's own art (the speech bubble, 54x46): the model, and proof this is a lobby scene
KEYBOARD = path_hash("STEAMMP_LOBBY_INPUT_KEYBOARD")  # src\natives.cpp kInputArtKeyboard
CONTROLLER = path_hash("STEAMMP_LOBBY_INPUT_CONTROLLER")  # src\natives.cpp kInputArtController
W, H = 54, 46
PAGE_W, PAGE_H = 128, 64
SLOTS = {KEYBOARD: (0, 0), CONTROLLER: (64, 0)}
SS = 4  # drawn 4x, then scaled down (smooth edges)
EDGE = (28, 30, 74, 255)    # the bubble's dark navy edge
TOP, BOTTOM = (255, 255, 255), (208, 210, 220)  # the bubble's white, shaded to light grey at the bottom


def shade(mask: Image.Image) -> Image.Image:
    """A white-to-grey body (the bubble's) inside `mask` (L, drawn size), with a dark edge around it."""
    w, h = mask.size
    grad = np.zeros((h, w, 4), np.uint8)
    for y in range(h):
        t = y / (h - 1)
        grad[y, :, :3] = [round(a + (b - a) * t) for a, b in zip(TOP, BOTTOM)]
    grad[:, :, 3] = 255
    body = Image.fromarray(grad)
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    edge = mask.filter(__import__("PIL.ImageFilter", fromlist=["MaxFilter"]).MaxFilter(2 * SS + 1))
    out.paste(Image.new("RGBA", (w, h), EDGE), (0, 0), edge)
    out.paste(body, (0, 0), mask)
    return out


def keyboard() -> np.ndarray:
    """A keyboard: a rounded slab with three rows of keys and a space bar cut in navy."""
    w, h = W * SS, H * SS
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    x0, y0, x1, y1 = 4 * SS, 10 * SS, (W - 4) * SS, (H - 9) * SS
    d.rounded_rectangle((x0, y0, x1, y1), radius=4 * SS, fill=255)
    img = shade(mask)
    k = ImageDraw.Draw(img)
    kw, gap = 4.2 * SS, 1.6 * SS
    for r in range(3):
        y = y0 + 3 * SS + r * 6 * SS
        cols = 7 - (r % 2)
        left = (x0 + x1) / 2 - (cols * kw + (cols - 1) * gap) / 2  # (centred: the odd row sits between the others' keys)
        for c in range(cols):
            x = left + c * (kw + gap)
            k.rectangle((x, y, x + kw, y + 3.5 * SS), fill=EDGE)
    k.rectangle((x0 + 12 * SS, y1 - 6 * SS, x1 - 12 * SS, y1 - 3 * SS), fill=EDGE)  # the space bar
    return np.array(img.resize((W, H), Image.LANCZOS))


def controller() -> np.ndarray:
    """A game pad: a wide body with two grips, a d-pad on the left and four buttons on the right, cut in navy."""
    w, h = W * SS, H * SS
    mask = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle((6 * SS, 11 * SS, (W - 6) * SS, 30 * SS), radius=9 * SS, fill=255)  # the body
    d.ellipse((3 * SS, 16 * SS, 21 * SS, 39 * SS), fill=255)                             # the grips
    d.ellipse(((W - 21) * SS, 16 * SS, (W - 3) * SS, 39 * SS), fill=255)
    img = shade(mask)
    k = ImageDraw.Draw(img)
    cx, cy, a, b = 15 * SS, 21 * SS, 6 * SS, 2 * SS  # the d-pad
    k.rectangle((cx - a, cy - b, cx + a, cy + b), fill=EDGE)
    k.rectangle((cx - b, cy - a, cx + b, cy + a), fill=EDGE)
    bx, by, s, r = (W - 15) * SS, 21 * SS, 4.5 * SS, 2.3 * SS  # the four buttons
    for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0)):
        x, y = bx + dx * s, by + dy * s
        k.ellipse((x - r, y - r, x + r, y + r), fill=EDGE)
    return np.array(img.resize((W, H), Image.LANCZOS))


def page_dds() -> tuple:
    """(A8R8G8B8 DDS of the new page, its pixels)."""
    page = np.zeros((PAGE_H, PAGE_W, 4), np.uint8)
    for key, a in ((KEYBOARD, keyboard()), (CONTROLLER, controller())):
        x, y = SLOTS[key]
        page[y:y + H, x:x + W] = a
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x100F, PAGE_H, PAGE_W, PAGE_W * 4, 0, 1)
    struct.pack_into("<8I", header, 76, 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    struct.pack_into("<I", header, 108, 0x1000)
    return bytes(header) + page[:, :, [2, 1, 0, 3]].tobytes(), page


def records() -> list:
    """The sprite rows {hash, u0 v0 u1 v0 u1 v1 u0 v1, page 0 of the new pages}."""
    rows = []
    for key, (x, y) in SLOTS.items():
        u0, v0, u1, v1 = x / PAGE_W, y / PAGE_H, (x + W) / PAGE_W, (y + H) / PAGE_H
        rows.append((key, u0, v0, u1, v0, u1, v1, u0, v1, 0))
    return rows


def sprite_pixels(scene: bytes, key: int) -> np.ndarray:
    """A sprite's pixels (RGBA) from the first PTEX chunk that has it."""
    for si in (3, 0):
        for _, c in chunks(sections(scene, "<")[si]):
            if c[:4] != b"PTEX":
                continue
            sp, tex = ptex_info(c)
            row = next((s for s in sp if s[0] == key), None)
            if row is None:
                continue
            img = Image.open(io.BytesIO(tex[row[9]])).convert("RGBA")
            w, h = img.size
            return np.array(img.crop((round(row[1] * w), round(row[2] * h), round(row[5] * w), round(row[6] * h))))
    raise KeyError(f"{key:08x}")


def add_icons(base: bytes) -> bytes:
    """A lobby scene (any layout, after the other lobby tools) with the two device pictures merged into the PTEX chunk that has
    the speech bubble."""
    dds, page = page_dds()
    ss = list(sections(base, "<"))
    merged = 0
    for si in (3, 0):
        cc = [c for _, c in chunks(ss[si])]
        rr = [c for _, c in chunks(ss[si + 1])]
        assert len(cc) == len(rr)
        for i, c in enumerate(cc):
            if c[:4] == b"PTEX" and not merged:
                keys = {s[0] for s in ptex_info(c)[0]}
                if BUBBLE not in keys:
                    continue
                assert not keys & set(SLOTS), "the scene already has the device pictures"
                cc[i], rr[i], _ = merge_ptex(c, rr[i], records(), [dds])
                merged += 1
                check_relos([b"", b"", b"", cc[i], rr[i], b""])
        ss[si] = b"".join(cc)
        ss[si + 1] = b"".join(rr)
    assert merged == 1, f"merged {merged} (no PTEX chunk with the speech bubble?)"
    out = pack(ss)
    for key, (x, y) in SLOTS.items():  # read back
        got = sprite_pixels(out, key)
        assert got.shape == (H, W, 4) and (got == page[y:y + H, x:x + W]).all(), f"{key:08x} reads back wrong"
    assert sprite_pixels(out, BUBBLE).shape == sprite_pixels(base, BUBBLE).shape, "the bubble moved"
    return out


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    _, page = page_dds()
    bubble = np.array(Image.open(root / "re" / "m7-voice" / "out" / "s_24064704.png").convert("RGBA"))
    strip = np.zeros((H, W * 3 + 20, 4), np.uint8)
    strip[:, :W] = page[0:H, 0:W]
    strip[:, W + 10:2 * W + 10] = page[0:H, 64:64 + W]
    if bubble.shape[:2] == (H, W):
        strip[:, 2 * W + 20:] = bubble
    big = Image.fromarray(strip).resize((strip.shape[1] * 4, H * 4), Image.NEAREST)
    bg = Image.new("RGBA", big.size, (24, 70, 170, 255))  # (about a lobby bar's blue)
    bg.alpha_composite(big)
    out = root / "build" / "lobby_input_icons_preview.png"
    bg.save(out)
    print(out)
    print(f"keyboard {KEYBOARD:08x}, controller {CONTROLLER:08x}")
