r"""Mod icons on the online game list (1.1 ENABLE MODIFICATIONS; the user's request 2026-10-24; re\m9-modifications\ICONS-FINDINGS.md).

Each game-list row's SIGNAL cell (row node 5; the console's TU2 drew its DLC column there) shows which supported mods the lobby
uses: EGG (Egg Hangar), METAL (Metal Sonic) or BOTH (Egg then Metal); a lobby without them keeps the stock signal bars. The
sprites are 100x50, a single icon right-aligned; the 8 cells are widened from 50 to 100 px to the left (their KFRM size and key x
floats, in place: the right edge stays). The three sprites live on one new uncompressed page (A8R8G8B8 256x128) merged like
egg_lobby's card (Codex's merge_ptex). Art: Metal Sonic's racer portrait e76b1254 (from Metal Sonic's own version of the scene,
its pixels as they are, 50x50 around its ring); Egg Hangar has no icon anywhere (an addition): the Death Egg from its lobby card
b2a1f58a, scaled into a 40-px circle with a white ring and a dark edge like the portraits'.

    import mod_icons; payload = mod_icons.add_icons(payload, metal_scene)   (metal_scene: Metal Sonic's version of the scene)
    python tools\mod_icons.py     a preview of the three sprites (build\mod_icons_preview.png) and their hashes
Needs numpy and Pillow (run without -I, as footer_keys.py).
"""
import colorsys
import io
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "codexui"))
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
import egg_lobby  # noqa: E402
from build_ui import check_relos, merge_ptex, merge_text, pack, path_hash, ptex_info  # noqa: E402
from decode_ui_texture import sections  # noqa: E402
from probe import chunks, text_info  # noqa: E402

PORTRAIT, CARD = 0xE76B1254, egg_lobby.CARD
TICK = 0x9309F511    # the console's DLC-column tick (a white tick in a green circle; in every PC lobby scene): the marks' model
X_HUE, DASH_HUE = 0 / 360, 34 / 360  # red (not installed here), orange (another version here)
DASH_VALUE, X_VALUE = 1.45, 1.25  # the tick's green is dark: orange at the same value reads brown, red reads dull on the red rows
OVERLAY = 190        # the marks' opacity over an icon (0-255)
# Each used mod's state on this game, Egg then Metal: N not used by the lobby, K the same build, D another version, X not installed.
STATE_PAIRS = [(e, m) for e in "NKDX" for m in "NKDX" if (e, m) != ("N", "N")]


def art_name(e: str, m: str) -> int:
    """A row state's sprite hash (src\\natives.cpp kNatRowMods builds the same name)."""
    return path_hash(f"STEAMMP_MODS_{e}{m}")


# The SIGNAL column's header (nodes 8935d3e3, 9b3a9173, 7791ab5d; the console's TU2 put "DLC" there): our text key, per language.
HEADER_NODES = (0x8935D3E3, 0x9B3A9173, 0x7791AB5D)
HEADER_KEY = path_hash("STEAMMP_MODS_COLUMN")
HEADER_TEXT = {"EN": b"MODS", "US": b"MODS", "JP": b"MODS", "FR": b"MODS", "GE": b"MODS", "IT": b"MOD", "SP": b"MODS"}
SIGNAL = 0x7BF528A4  # the cells' stock sprite (four green bars)
CELLS = (0x37D7E2E7, 0xFBF2B2DE, 0xE4418BBD, 0x53C56022, 0x91785224, 0xD2FE7054, 0x83166898, 0xE4E8720D)  # rows 0-7, node 5
UNIT = 2048.0        # KFRM sizes: pixels / 2048 (the cells' 50 px = .0244)
OLD_W, NEW_W = 50, 100
ICON = 50
PAGE_W, PAGE_H = 256, 512
# A lobby without supported mods: the stock signal bars, right-aligned in a 100x50 sprite (the stock 50x50 one stretched across the
# widened cell; the user's mods.4 test).
SIGNAL_WIDE = art_name("N", "N")
SLOTS = {art_name(e, m): (128 * (i % 2), 64 * (i // 2)) for i, (e, m) in enumerate(STATE_PAIRS + [("N", "N")])}  # each sprite's top-left
EGG_CROP = (0, 0, 141, 141)  # the card's square around the Death Egg (x0, y0, x1, y1 in its 396x141 pixels)


def sprite_pixels(scene: bytes, key: int) -> np.ndarray:
    """A sprite's pixels (RGBA) from the first PTEX chunk of a scene's section 3."""
    ptex = next(c for _, c in chunks(sections(scene, "<")[3]) if c[:4] == b"PTEX")
    sp, tex = ptex_info(ptex)
    row = next(s for s in sp if s[0] == key)
    img = Image.open(io.BytesIO(tex[row[9]])).convert("RGBA")
    w, h = img.size
    box = (round(row[1] * w), round(row[2] * h), round(row[5] * w), round(row[6] * h))
    return np.array(img.crop(box))


def card_pixels() -> np.ndarray:
    """Egg Hangar's lobby card (the DLC's, as egg_lobby merges it)."""
    row, tex, _ = egg_lobby.console_card(egg_lobby.LANG_SOURCE["EN"])
    img = Image.open(io.BytesIO(tex[0])).convert("RGBA")
    w, h = img.size
    return np.array(img.crop((round(row[1] * w), round(row[2] * h), round(row[5] * w), round(row[6] * h))))


def egg_icon(card: np.ndarray) -> np.ndarray:
    """The Death Egg in a 40-px circle with a white ring and a dark edge, on a 50x50 transparent square."""
    inner = Image.fromarray(card).crop(EGG_CROP).resize((40, 40), Image.LANCZOS)
    out = Image.new("RGBA", (ICON, ICON), (0, 0, 0, 0))
    d = ImageDraw.Draw(out)
    d.ellipse((0, 0, 49, 49), fill=(60, 64, 80, 255))      # the dark edge and white ring, as the portraits'
    d.ellipse((1, 1, 48, 48), fill=(255, 255, 255, 255))
    mask = Image.new("L", (40, 40), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, 39, 39), fill=255)
    out.paste(inner, (5, 5), mask)
    return np.array(out)


def metal_icon(portrait: np.ndarray) -> np.ndarray:
    """Metal Sonic's portrait, 50x50 around its ring (visible circle x22-70, y2-53 of the 90x55 sprite)."""
    assert portrait.shape[:2] == (55, 90), f"portrait is {portrait.shape}"
    return portrait[2:52, 21:71].copy()


def badge(tick: np.ndarray, hue: float, glyph: str, value: float = 1.0) -> np.ndarray:
    """The console's DLC tick (9309f511, a white tick in a green circle) made into a white X (hue red) or dash (hue orange): the
    green moved to `hue`, the tick painted over with its row's fill, the glyph drawn white with a dark edge (the user's design)."""
    a = tick.astype(float) / 255
    out = tick.copy()
    h, w = tick.shape[:2]
    green = np.zeros((h, w), bool)
    for y in range(h):
        for x in range(w):
            r, g, b, al = a[y, x]
            hh, s, v = colorsys.rgb_to_hsv(r, g, b)
            green[y, x] = al > 0.9 and s > 0.35 and 0.2 < hh < 0.45
    cy, cx = (h - 1) / 2, (w - 1) / 2
    ys, xs = np.nonzero(green)
    radius = np.sqrt((ys - cy) ** 2 + (xs - cx) ** 2).max()
    fills = {}
    for y in range(h):  # the whole inside repainted with its row's green (the tick and its shadow go; the fill's gradient stays)
        row = tick[y][green[y]]
        if len(row):
            fills[y] = np.median(row, axis=0)
    for y in range(h):
        fill = fills.get(y, fills[min(fills, key=lambda k: abs(k - y))])
        for x in range(w):
            if (y - cy) ** 2 + (x - cx) ** 2 <= (radius - 0.5) ** 2:
                out[y, x] = fill
    for y in range(h):
        for x in range(w):
            r, g, b, al = out[y, x].astype(float) / 255
            hh, s, v = colorsys.rgb_to_hsv(r, g, b)
            if al > 0.5 and s > 0.35 and 0.2 < hh < 0.45:
                r2, g2, b2 = colorsys.hsv_to_rgb(hue, s, min(1.0, v * value))
                out[y, x] = (round(r2 * 255), round(g2 * 255), round(b2 * 255), out[y, x][3])
    img = Image.fromarray(out)
    d = ImageDraw.Draw(img)
    if glyph == "X":  # (strokes as thin as the tick's: white 4 px, a 1-px dark edge)
        m = radius * 0.42
        for width, colour in ((6, (30, 20, 40, 255)), (4, (255, 255, 255, 255))):
            d.line((cx - m, cy - m, cx + m, cy + m), fill=colour, width=width)
            d.line((cx - m, cy + m, cx + m, cy - m), fill=colour, width=width)
    else:
        m = radius * 0.5
        d.rectangle((round(cx - m - 1), round(cy - 3), round(cx + m + 1), round(cy + 2)), fill=(30, 20, 40, 255))
        d.rectangle((round(cx - m), round(cy - 2), round(cx + m), round(cy + 1)), fill=(255, 255, 255, 255))
    return np.array(img)


def circle_fit(img: np.ndarray) -> tuple:
    """(centre x, centre y, diameter) of a round icon's disc: its widest rows (so Metal Sonic's ears above the ring don't count)."""
    rows = []
    for y in range(img.shape[0]):
        xs = np.nonzero(img[y, :, 3] > 128)[0]
        if len(xs):
            rows.append((y, xs[0], xs[-1] + 1))
    d = max(x1 - x0 for _, x0, x1 in rows)
    widest = [(y, x0, x1) for y, x0, x1 in rows if x1 - x0 >= d - 1]
    return (np.mean([(x0 + x1) / 2 for _, x0, x1 in widest]), np.mean([y for y, _, _ in widest]) + 0.5, d)


def overlay(icon: np.ndarray, mark: np.ndarray) -> np.ndarray:
    """The mark's disc laid exactly over the icon's disc (the user's design) at OVERLAY opacity (the icon shows through)."""
    mx, my, md = circle_fit(mark)
    ix, iy, idm = circle_fit(icon)
    scale = idm / md
    w, h = mark.shape[1], mark.shape[0]
    top = Image.fromarray(mark).resize((round(w * scale), round(h * scale)), Image.LANCZOS)
    layer = Image.new("RGBA", (icon.shape[1], icon.shape[0]), (0, 0, 0, 0))
    layer.paste(top, (round(ix - mx * scale), round(iy - my * scale)))
    a = np.array(layer)
    a[:, :, 3] = (a[:, :, 3].astype(int) * OVERLAY // 255).astype(np.uint8)
    base = Image.fromarray(icon)
    base.alpha_composite(Image.fromarray(a))
    return np.array(base)


def sprites(egg: np.ndarray, metal: np.ndarray, tick: np.ndarray, signal: np.ndarray) -> dict:
    """{art hash: 100x50 RGBA} for every row state (art_name): each used mod's icon as it is (K), with the orange dash (D: another
    version here) or the red X (X: not installed here); a single icon right-aligned, two = Egg then Metal."""
    marks = {"K": None, "D": badge(tick, DASH_HUE, "-", DASH_VALUE), "X": badge(tick, X_HUE, "X", X_VALUE)}
    out = {}
    for e, m in STATE_PAIRS:
        icons = [overlay(icon, marks[s]) if marks[s] is not None else icon for icon, s in ((egg, e), (metal, m)) if s != "N"]
        a = np.zeros((ICON, NEW_W, 4), np.uint8)
        a[:, NEW_W - ICON * len(icons):] = np.concatenate(icons, axis=1)
        out[art_name(e, m)] = a
    assert signal.shape == (ICON, OLD_W, 4), signal.shape
    a = np.zeros((ICON, NEW_W, 4), np.uint8)
    a[:, NEW_W - OLD_W:] = signal
    out[SIGNAL_WIDE] = a
    return out


def page_dds(images: dict) -> tuple:
    """(A8R8G8B8 DDS of the new page, its pixels)."""
    page = np.zeros((PAGE_H, PAGE_W, 4), np.uint8)
    for key, a in images.items():
        x, y = SLOTS[key]
        page[y:y + a.shape[0], x:x + a.shape[1]] = a
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
        u0, v0, u1, v1 = x / PAGE_W, y / PAGE_H, (x + NEW_W) / PAGE_W, (y + ICON) / PAGE_H
        rows.append((key, u0, v0, u1, v0, u1, v1, u0, v1, 0))
    return rows


def widen_cells(kfrm: bytes) -> tuple:
    """The KFRM chunk with the 8 SIGNAL cells 100 px wide, right edge kept: (chunk, cells changed)."""
    out = bytearray(kfrm)
    n = struct.unpack_from("<I", out, 0x10)[0]
    done = 0
    for i in range(n):
        e = 0x14 + 0x14 * i
        h, off, cnt, w, hh = struct.unpack_from("<IIIff", out, e)
        if h not in CELLS:
            continue
        assert abs(w * UNIT - OLD_W) < 0.5 and abs(hh * UNIT - ICON) < 0.5, f"cell {h:08x} is {w * UNIT}x{hh * UNIT}"
        shift = (NEW_W - OLD_W) / UNIT
        struct.pack_into("<f", out, e + 12, NEW_W / UNIT)
        for j in range((cnt & 0xFFFF) or (cnt >> 16)):
            q = 0x10 + off + 0x28 * j + 12
            x = struct.unpack_from("<f", out, q)[0]
            struct.pack_into("<f", out, q, x - shift)
        done += 1
    return bytes(out), done


def all_sprites(base: bytes, metal_scene: bytes) -> dict:
    """Every row state's sprite (the tick from the scene being built, the portrait from Metal Sonic's)."""
    return sprites(egg_icon(card_pixels()), metal_icon(sprite_pixels(metal_scene, PORTRAIT)), sprite_pixels(base, TICK),
                   sprite_pixels(base, SIGNAL))


def add_icons(base: bytes, metal_scene: bytes, lang: str) -> bytes:
    """A game-list scene (any layout, after egg_lobby.add_card) with the mod icons, the widened SIGNAL cells and the MODS header."""
    dds, _ = page_dds(all_sprites(base, metal_scene))
    ss = list(sections(base, "<"))
    merged = texts = cells = 0
    for si in (3, 0):
        cc = [c for _, c in chunks(ss[si])]
        rr = [c for _, c in chunks(ss[si + 1])]
        assert len(cc) == len(rr)
        for i, c in enumerate(cc):
            if c[:4] == b"PTEX" and not merged:
                keys = {s[0] for s in ptex_info(c)[0]}
                assert SIGNAL in keys and TICK in keys and not keys & set(SLOTS), "the scene lacks the signal art or the tick, or has the icons"
                cc[i], rr[i], _ = merge_ptex(c, rr[i], records(), [dds])
                merged += 1
                check_relos([b"", b"", b"", cc[i], rr[i], b""])
            elif c[:4] == b"TEXT" and not texts:
                assert HEADER_KEY not in dict(text_info(c)), "the scene already has the MODS header"
                cc[i], rr[i], _ = merge_text(c, rr[i], {HEADER_KEY: HEADER_TEXT[lang]})
                texts += 1
                check_relos([b"", b"", b"", cc[i], rr[i], b""])
            elif c[:4] == b"KFRM":
                cc[i], k = widen_cells(c)
                cells += k
        ss[si] = b"".join(cc)
        ss[si + 1] = b"".join(rr)
    assert merged == 1 and texts == 1 and cells and cells % len(CELLS) == 0, f"merged {merged}, texts {texts}, cells {cells}"
    out = pack(ss)
    check(out, lang)
    return out


def check(scene: bytes, lang: str) -> None:
    """The icons read back with their pixels, the header text is there; every SIGNAL cell is 100 px wide."""
    for key, (x, y) in SLOTS.items():
        assert sprite_pixels(scene, key).shape == (ICON, NEW_W, 4), f"{key:08x}"
    assert any(c[:4] == b"TEXT" and dict(text_info(c)).get(HEADER_KEY) == HEADER_TEXT[lang]
               for si in (3, 0) for _, c in chunks(sections(scene, "<")[si])), "the MODS header text"
    for si in (3, 0):
        for _, c in chunks(sections(scene, "<")[si]):
            if c[:4] == b"KFRM":
                n = struct.unpack_from("<I", c, 0x10)[0]
                for i in range(n):
                    h, _, _, w, _ = struct.unpack_from("<IIIff", c, 0x14 + 0x14 * i)
                    assert h not in CELLS or abs(w * UNIT - NEW_W) < 0.5, f"cell {h:08x} is {w * UNIT} wide"


if __name__ == "__main__":
    root = Path(__file__).resolve().parents[1]
    metal = (root / "reference" / "navigator-bases" / "metal-sonic-1.1.0-beta.3" / "E01CF5DC.dat").read_bytes()
    _, page = page_dds(all_sprites(metal, metal))
    big = Image.fromarray(page).resize((PAGE_W * 2, PAGE_H * 2), Image.NEAREST)
    bg = Image.new("RGBA", big.size, (150, 20, 20, 255))  # (a game-list row's red)
    bg.alpha_composite(big)
    out = root / "build" / "mod_icons_preview.png"
    bg.save(out)
    print(out)
    for e, m in STATE_PAIRS:
        print(f"{e}{m} {art_name(e, m):08x}")
