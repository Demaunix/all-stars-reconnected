r"""RANDOM racer / RANDOM COURSE art and text in the online lobby's scenes (an addition the original never had; the user's request
2026-11-09, notes\RANDOM-SLOTS.md).

The picture is the game's own blue "???" (sprite 48bbdf57, 135x135, an unused profile picker's default image in
fe_character_select_sub_EN, member AFB7E20B of this PC's game archive), decoded at build time: no game data is committed.
- Popups scenes (fe_net_lobby_popups_<LANG>, the CHANGE RACER popup): the racer-portrait sprite 5abe4516 (248x236, the portrait
  slot 20a43a61; the "???" scaled to fill it with a thin black frame, as the game draws it, the user's Q5 "cut to the portrait shape"),
  the racer name "RANDOM" (4327702e, both name layers) and the Settings popup's course name (a560e6d8, also "RANDOM").
- Lobby scenes (fe_net_lobby_<LANG>): the course card d9b102b7 (396x141, a soft band of the "???" across the whole card, no
  frame) and the course name (a560e6d8, "RANDOM").
Each sprite lives on one new uncompressed A8R8G8B8 page (as mod_icons), its edge pixels repeated two pixels outward so filtering
never blends in the empty page. The words are our translations (the game has no RANDOM text; JP/US get English, as for Egg).

    import random_art; payload = random_art.add_popup(payload, lang); payload = random_art.add_lobby(payload, lang)
    python tools\random_art.py     previews (build\random_art\*.png) and the hashes
Needs numpy and Pillow (run without -I, as footer_keys.py).
"""
import io
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "codexui"))
import numpy as np  # noqa: E402
from functools import lru_cache  # noqa: E402
from PIL import Image  # noqa: E402
import mod_icons  # noqa: E402
from build_ui import check_relos, merge_ptex, merge_text, pack  # noqa: E402
from decode_ui_texture import sections  # noqa: E402
from probe import chunks, text_info  # noqa: E402

# The pictures are the user's own HD "???" (reference\random-art\README.md): the game's own (sprite f619d6c0 / 48bbdf57) has its
# outer question marks cut off and a baked-in frame on every version (re\random-art-hunt\, 2026-11-09).
ART = Path(__file__).resolve().parents[1] / "reference" / "random-art"
HD, CARD_HD = ART / "question-marks-hd.webp", ART / "course-card-hd.webp"
INPUTS = ("reference/random-art/question-marks-hd.webp", "reference/random-art/course-card-hd.webp")  # (footer_keys / package)
# The game picture's two light glints, in HD-picture pixels (README): the right "?"'s top corner, the middle "?"'s left edge.
GLINTS = ((1515, 761, 1.0), (795, 1095, 0.9))  # (x, y, strength); the user boxed both on the game picture (2026-11-09)
GLINT_RAY = 150                               # ray length in HD pixels (the game's ~15 px at 9.2 HD px each)
CARD_SCALE, CARD_X, CARD_Y = 1.10, 64, 552    # card px per HD px; the card's (0,0) in the HD picture
PORTRAIT = 0x5ABE4516     # name_hash('steammp_random_portrait')   (tools\netmenu\racer.py RANDOM_PORTRAIT)
RACER_TEXT = 0x4327702E   # name_hash('STEAMMP_RANDOM')            (racer.py RANDOM_TEXT)
CARD = 0xD9B102B7         # name_hash('steammp_random_course_art') (tools\netmenu\settings.py RANDOM_ART)
COURSE_TEXT = 0xA560E6D8  # name_hash('STEAMMP_RANDOM_COURSE')     (settings.py RANDOM_NAME)
PORTRAIT_W, PORTRAIT_H = 248, 236  # the popup's portrait slot (every racer's portrait sprite is this size)
CARD_W, CARD_H = 396, 141          # a lobby course card (Egg Hangar's b2a1f58a is this size)
EDGE = 2                          # edge pixels repeated outward on the page

RACER = {"EN": "RANDOM", "FR": "ALÉATOIRE", "GE": "ZUFALL", "IT": "CASUALE", "SP": "ALEATORIO"}
RACER["JP"] = RACER["US"] = RACER["EN"]
# The course is "RANDOM" over "RANDOM" (the user, 2026-11-09 in-game test: the Settings popup's COURSE row showed "RANDOM" over
# "COURSE"; "I want RANDOM to be on top of RANDOM", in both scenes).
# rs2 in-game look (the user, 2026-11-09 05:40): the lobby's settings panel shows ONE "RANDOM" (two lines spilled into its LAPS
# row); the Settings popup's COURSE row keeps "RANDOM" over "RANDOM".
COURSE_TWO_LINES = {lang: word + "\n" + word for lang, word in RACER.items()}
COURSE = dict(RACER)
PORTRAIT_TURN = 12.0  # degrees counter-clockwise for the racer tile's "???" (the user, rs2 look: "rotated counter clockwise a
                      # little"; picked 12 of the 0/5/8/12 previews)


def with_glints(img: Image.Image, scale: float = 1.0, dx: float = 0.0, dy: float = 0.0) -> Image.Image:
    """`img` with the game's light glints drawn back (the user, 2026-11-09): a soft white four-point star (long upright and level
    rays, short diagonal ones, a small glow) at each GLINTS spot, mapped by (HD - (dx, dy)) * scale."""
    w, h = img.size
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    glow = np.zeros((h, w), np.float32)
    for gx, gy, k in GLINTS:
        x, y, r = (gx - dx) * scale, (gy - dy) * scale, GLINT_RAY * scale * k
        ax, ay = np.abs(xx - x), np.abs(yy - y)
        d1, d2 = np.abs((xx - x) + (yy - y)) / 1.414, np.abs((xx - x) - (yy - y)) / 1.414  # distance from the diagonals
        thin = max(1.0, r / 28)
        star = (np.exp(-(ax / thin) ** 2) * np.clip(1 - ay / r, 0, 1) ** 2 + np.exp(-(ay / thin) ** 2) * np.clip(1 - ax / r, 0, 1) ** 2
                + 0.5 * (np.exp(-(d1 / thin) ** 2) + np.exp(-(d2 / thin) ** 2)) * np.clip(1 - np.hypot(ax, ay) / (0.7 * r), 0, 1) ** 2)
        star += np.exp(-(ax ** 2 + ay ** 2) / (2 * (r / 7) ** 2))
        glow = np.maximum(glow, np.clip(star, 0, 1) * k)
    a = np.array(img.convert("RGBA")).astype(np.float32)
    g = glow[..., None]
    alpha = a[..., 3:4] / 255
    out_a = alpha + g * (1 - alpha)                            # white light laid over the picture ("over", white)
    rgb = (a[..., :3] * alpha * (1 - g) + 255 * g) / np.maximum(out_a, 1e-6)
    return Image.fromarray(np.dstack([rgb, out_a * 255]).clip(0, 255).astype(np.uint8))


@lru_cache(maxsize=None)
def portrait(margin: int = 4, turn: float = PORTRAIT_TURN) -> np.ndarray:
    """The racer-portrait sprite: the user's HD "???" whole with the game's glints, turned `turn` degrees counter-clockwise,
    fitted inside the 248x236 slot on a transparent background, no frame (the user, 2026-11-09). Turned and scaled
    premultiplied, so the see-through edges get no dark fringe."""
    img = with_glints(Image.open(HD).convert("RGBA"))
    if turn:
        img = img.convert("RGBa").rotate(turn, Image.BICUBIC, expand=True).convert("RGBA")
    img = img.crop(img.getchannel("A").point(lambda v: 255 if v > 8 else 0).getbbox())
    scale = min((PORTRAIT_W - 2 * margin) / img.width, (PORTRAIT_H - 2 * margin) / img.height)
    size = (round(img.width * scale), round(img.height * scale))
    small = img.convert("RGBa").resize(size, Image.LANCZOS).convert("RGBA")
    out = Image.new("RGBA", (PORTRAIT_W, PORTRAIT_H), (0, 0, 0, 0))
    out.alpha_composite(small, ((PORTRAIT_W - size[0]) // 2, (PORTRAIT_H - size[1]) // 2))
    return np.array(out)


@lru_cache(maxsize=None)
def card() -> np.ndarray:
    """The course card: the user's HD card picture (2.81:1, as the card) with the game's glints, scaled to 396x141, no frame
    (the user, 2026-11-09: "Use this for the race track icon")."""
    img = with_glints(Image.open(CARD_HD).convert("RGBA"), CARD_SCALE, CARD_X, CARD_Y)
    return np.array(img.resize((CARD_W, CARD_H), Image.LANCZOS))


def page(sprite: np.ndarray, w: int, h: int) -> tuple:
    """(A8R8G8B8 DDS of a w x h page with the sprite at (EDGE, EDGE), its edge pixels repeated EDGE px outward; the sprite row)."""
    sh, sw = sprite.shape[:2]
    assert sw + 2 * EDGE <= w and sh + 2 * EDGE <= h
    pix = np.zeros((h, w, 4), np.uint8)
    pix[:sh + 2 * EDGE, :sw + 2 * EDGE] = np.pad(sprite, ((EDGE, EDGE), (EDGE, EDGE), (0, 0)), mode="edge")
    header = bytearray(128)
    header[:4] = b"DDS "
    struct.pack_into("<7I", header, 4, 124, 0x100F, h, w, w * 4, 0, 1)
    struct.pack_into("<8I", header, 76, 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    struct.pack_into("<I", header, 108, 0x1000)
    u0, v0, u1, v1 = EDGE / w, EDGE / h, (EDGE + sw) / w, (EDGE + sh) / h
    return bytes(header) + pix[:, :, [2, 1, 0, 3]].tobytes(), (u0, v0, u1, v0, u1, v1, u0, v1, 0)


def merge(base: bytes, key: int, sprite: np.ndarray, w: int, h: int, texts: dict) -> bytes:
    """`base` with the sprite (on its own new page) and the texts added; refuses a scene that already has any of them."""
    dds, uv = page(sprite, w, h)
    ss = list(sections(base, "<"))
    merged = done = 0
    for si in (3, 0):
        cc = [c for _, c in chunks(ss[si])]
        rr = [c for _, c in chunks(ss[si + 1])]
        assert len(cc) == len(rr)
        for i, c in enumerate(cc):
            if c[:4] == b"PTEX" and not merged:
                cc[i], rr[i], _ = merge_ptex(c, rr[i], [(key, *uv)], [dds])  # (asserts the key is new)
                merged += 1
                check_relos([b"", b"", b"", cc[i], rr[i], b""])
            elif c[:4] == b"TEXT" and not done:
                assert not set(texts) & set(dict(text_info(c))), "the scene already has a RANDOM text"
                cc[i], rr[i], _ = merge_text(c, rr[i], texts)
                done += 1
                check_relos([b"", b"", b"", cc[i], rr[i], b""])
        ss[si] = b"".join(cc)
        ss[si + 1] = b"".join(rr)
    assert merged == 1 and done == 1, f"PTEX {merged}, TEXT {done}"
    out = pack(ss)
    check(out, key, sprite, texts)
    return out


def check(scene: bytes, key: int, sprite: np.ndarray, texts: dict) -> None:
    """The sprite reads back with exactly its pixels, every text is there as given."""
    got = mod_icons.sprite_pixels(scene, key)
    assert got.shape == sprite.shape and np.array_equal(got, sprite), f"{key:08x} pixels"
    found = {}
    for si in (3, 0):
        for _, c in chunks(sections(scene, "<")[si]):
            if c[:4] == b"TEXT":
                found.update({k: v for k, v in text_info(c) if k in texts})
    assert found == texts, f"texts {found}"


def popup_texts(lang: str) -> dict:
    return {RACER_TEXT: RACER[lang].encode("utf-8"), COURSE_TEXT: COURSE_TWO_LINES[lang].encode("utf-8")}


def lobby_texts(lang: str) -> dict:
    return {COURSE_TEXT: COURSE[lang].encode("utf-8")}


def add_popup(base: bytes, lang: str) -> bytes:
    """A lobby popups scene with the RANDOM portrait, the word RANDOM and the RANDOM COURSE two-line name."""
    return merge(base, PORTRAIT, portrait(), 256, 256, popup_texts(lang))


def add_lobby(base: bytes, lang: str) -> bytes:
    """A lobby scene with the RANDOM COURSE card and its one-line name."""
    return merge(base, CARD, card(), 512, 256, lobby_texts(lang))


if __name__ == "__main__":
    from netmenu.pkg import name_hash
    assert name_hash("steammp_random_portrait") == PORTRAIT and name_hash("STEAMMP_RANDOM") == RACER_TEXT
    assert name_hash("steammp_random_course_art") == CARD and name_hash("STEAMMP_RANDOM_COURSE") == COURSE_TEXT
    out = Path(__file__).resolve().parents[1] / "build" / "random_art"
    out.mkdir(parents=True, exist_ok=True)
    Image.fromarray(portrait()).save(out / "portrait.png")
    Image.fromarray(card()).save(out / "card.png")
    print(out, "%08x %08x %08x %08x" % (PORTRAIT, RACER_TEXT, CARD, COURSE_TEXT))
