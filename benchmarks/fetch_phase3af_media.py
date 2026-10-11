# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Download openly licensed photographs for the Phase 3A-F policy corpus.

The search list, quality cycle, and 4K eligibility rule are fixed here.
Nothing in this script reads decoder timings. YUY2 frames are gitignored.
photos.tsv is the provenance record. These frames are re-encoded later; they
are not native camera CineForm.
"""
import json
import time
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MEDIA = ROOT / "benchmarks" / "phase3af_media"
API = "https://commons.wikimedia.org/w/api.php"
UA = "HyperCinePhase3AF/1.0 (local codec research; contact: none)"
TARGET = 32
MAX_4K = 8
QUALITIES = ["low", "medium", "high", "filmscan1", "filmscan2", "filmscan3"]
PHOTO_SEED = "0x3AF51A77"

# Title keywords are a predeclared subject check. A search hit is accepted only
# when its title contains one of these stems. This rejects unrelated Commons hits
# before any encode or timing.
KEYWORDS = {
    "brick_wall": ("brick",),
    "circuit_board": ("circuit", "pcb"),
    "handwriting": ("handwrit", "manuscript"),
    "foliage": ("foliage", "forest", "leaf", "leaves", "tree"),
    "grass": ("grass", "meadow", "lawn"),
    "water_surface": ("water", "lake", "river", "ripple"),
    "ocean_waves": ("ocean", "wave", "sea"),
    "textile": ("textile", "fabric", "woven", "cloth"),
    "night_city": ("night", "skyline"),
    "open_sky": ("sky", "cloud"),
    "portrait": ("portrait", "face"),
    "flower": ("flower", "petal", "bloom"),
    "wood_grain": ("wood", "grain", "timber"),
    "sand": ("sand", "beach"),
    "rust": ("rust",),
    "marble": ("marble",),
    "rain": ("rain",),
    "fur": ("fur",),
    "snow": ("snow",),
    "desert": ("desert", "dune"),
    "mountain": ("mountain",),
    "street": ("street",),
    "concrete": ("concrete",),
    "roof_tile": ("tile", "roof"),
    "books": ("book",),
    "keyboard": ("keyboard",),
    "map": ("map",),
    "painting": ("painting", "canvas"),
    "sculpture": ("sculpture", "statue"),
    "bicycle": ("bicycle", "bike"),
    "window": ("window",),
    "forest_path": ("forest", "trail", "path"),
    "waterfall": ("waterfall", "cascade"),
    "rock": ("rock", "stone"),
    "leaves": ("leaf", "leaves"),
    "clouds": ("cloud",),
    "food": ("food", "meal"),
    "animal": ("animal", "bird", "dog", "cat", "horse", "deer"),
    "bridge": ("bridge",),
    "stairs": ("stair",),
    "graffiti": ("graffiti",),
    "market": ("market",),
    "harbor": ("harbor", "harbour", "boat"),
    "fence": ("fence",),
    "moss": ("moss",),
}

# family, primary search, fallback search, workload tag.
# The tag is a property of the query name, chosen before any pixel is seen.
SEARCHES = [
    ("brick_wall", "incategory:CC-Zero brick wall", "brick wall texture", "sparse_structured"),
    ("circuit_board", "incategory:CC-Zero circuit board", "printed circuit board macro", "high_entropy"),
    ("handwriting", "incategory:CC-Zero handwriting", "handwritten letter scan", "sparse_structured"),
    ("foliage", "incategory:CC-Zero forest leaves", "forest foliage close", "high_entropy"),
    ("grass", "incategory:CC-Zero lawn grass", "meadow grass close-up", "high_entropy"),
    ("water_surface", "incategory:CC-Zero lake water", "water surface ripples", "high_entropy"),
    ("ocean_waves", "incategory:CC-Zero sea waves", "ocean wave close", "high_entropy"),
    ("textile", "incategory:CC-Zero woven textile", "woven fabric detail", "high_entropy"),
    ("night_city", "incategory:CC-Zero city night", "city skyline night", "sparse_structured"),
    ("open_sky", "incategory:CC-Zero blue sky", "clear sky clouds", "sparse_structured"),
    ("portrait", "incategory:CC-Zero human face", "portrait face photograph", "sparse_structured"),
    ("flower", "incategory:CC-Zero flower macro", "flower petal macro", "high_entropy"),
    ("wood_grain", "incategory:CC-Zero wood grain", "wood grain texture", "high_entropy"),
    ("sand", "incategory:CC-Zero sand beach", "sand texture close", "high_entropy"),
    ("rust", "incategory:CC-Zero rust metal", "rusty metal texture", "high_entropy"),
    ("marble", "incategory:CC-Zero marble stone", "marble surface texture", "sparse_structured"),
    ("rain", "incategory:CC-Zero rain drops", "rain on glass", "high_entropy"),
    ("fur", "incategory:CC-Zero animal fur", "fur texture close", "high_entropy"),
    ("snow", "incategory:CC-Zero snow field", "snow landscape", "sparse_structured"),
    ("desert", "incategory:CC-Zero desert dune", "sand dune landscape", "neutral"),
    ("mountain", "incategory:CC-Zero mountain landscape", "mountain ridge landscape", "neutral"),
    ("street", "incategory:CC-Zero street photography", "urban street photograph", "neutral"),
    ("concrete", "incategory:CC-Zero concrete wall", "concrete texture", "sparse_structured"),
    ("roof_tile", "incategory:CC-Zero roof tiles", "ceramic roof tiles", "sparse_structured"),
    ("books", "incategory:CC-Zero books", "open book pages", "sparse_structured"),
    ("keyboard", "incategory:CC-Zero computer keyboard", "keyboard close-up", "sparse_structured"),
    ("map", "incategory:CC-Zero map", "printed map detail", "sparse_structured"),
    ("painting", "incategory:CC-Zero oil painting", "painting canvas detail", "neutral"),
    ("sculpture", "incategory:CC-Zero sculpture", "stone sculpture", "neutral"),
    ("bicycle", "incategory:CC-Zero bicycle", "bicycle photograph", "neutral"),
    ("window", "incategory:CC-Zero window glass", "window facade", "sparse_structured"),
    ("forest_path", "incategory:CC-Zero forest path", "forest trail", "high_entropy"),
    ("waterfall", "incategory:CC-Zero waterfall", "waterfall photograph", "high_entropy"),
    ("rock", "incategory:CC-Zero rock texture", "stone rock close", "neutral"),
    ("leaves", "incategory:CC-Zero autumn leaves", "leaf litter", "high_entropy"),
    ("clouds", "incategory:CC-Zero cloudscape", "cumulus clouds", "sparse_structured"),
    ("food", "incategory:CC-Zero food", "food photograph", "neutral"),
    ("animal", "incategory:CC-Zero animal", "wild animal photograph", "neutral"),
    ("bridge", "incategory:CC-Zero bridge", "bridge architecture", "neutral"),
    ("stairs", "incategory:CC-Zero stairs", "staircase photograph", "sparse_structured"),
    ("graffiti", "incategory:CC-Zero graffiti", "wall graffiti", "neutral"),
    ("market", "incategory:CC-Zero market", "outdoor market", "neutral"),
    ("harbor", "incategory:CC-Zero harbor boats", "harbor photograph", "neutral"),
    ("fence", "incategory:CC-Zero fence", "metal fence", "sparse_structured"),
    ("moss", "incategory:CC-Zero moss", "moss texture", "high_entropy"),
]

HEADER = (
    "id\tfamily\twidth\theight\tpath\tlicense\tsource_url\tsource_identity\t"
    "quality\tseed\tprovenance\tworkload_tag\tpreviously_timed\n"
)


def get(url, attempts=4):
    delay = 2.0
    last = None
    for _ in range(attempts):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": UA})
            with urllib.request.urlopen(req, timeout=90) as response:
                return response.read()
        except Exception as exc:  # noqa: BLE001 - Commons rate limits and timeouts are retried.
            last = exc
            time.sleep(delay)
            delay *= 2
    raise RuntimeError(f"download failed: {url} ({last})")


def thumbnail(url, width):
    base = url.split("?", 1)[0]
    marker = "/wikipedia/commons/"
    pos = base.find(marker)
    if pos < 0 or "/thumb/" in base:
        return None
    rest = base[pos + len(marker):]
    name = rest.rsplit("/", 1)[-1]
    thumb_name = name + ".jpg" if name.lower().endswith((".tif", ".tiff", ".png", ".webp")) else name
    return base[:pos] + "/wikipedia/commons/thumb/" + rest + f"/{width}px-" + thumb_name


def license_name(meta):
    short = meta.get("LicenseShortName", {}).get("value", "")
    terms = meta.get("UsageTerms", {}).get("value", "")
    return f"{short} {terms}".strip()


def acceptable(name):
    text = name.lower()
    if any(flag in text for flag in ("nc", "nd", "noncommercial", "no derivative", "gfdl")):
        return False
    return any(flag in text for flag in ("cc0", "cc-zero", "public domain", "cc by", "cc-by"))


def prior_text():
    parts = []
    media = ROOT / "benchmarks" / "phase3ad_media"
    for name in ("validation_manifest.tsv", "holdout_manifest.tsv", "holdout_titles.txt"):
        path = media / name
        if path.exists():
            parts.append(path.read_text(encoding="utf-8").lower())
    return "\n".join(parts)


def search(query):
    params = {
        "action": "query",
        "generator": "search",
        "gsrsearch": query,
        "gsrnamespace": "6",
        "gsrlimit": "12",
        "prop": "imageinfo",
        "iiprop": "url|size|mime|extmetadata",
        "format": "json",
    }
    payload = json.loads(get(API + "?" + urllib.parse.urlencode(params)))
    pages = payload.get("query", {}).get("pages", {})
    found = []
    for page in pages.values():
        info = (page.get("imageinfo") or [None])[0]
        if not info or not str(info.get("mime", "")).startswith("image/"):
            continue
        if info.get("width", 0) < 1600 or info.get("height", 0) < 900:
            continue
        name = license_name(info.get("extmetadata") or {})
        if not acceptable(name):
            continue
        found.append({
            "title": page.get("title", ""),
            "url": info.get("url", ""),
            "license": name,
            "width": int(info.get("width", 0)),
            "height": int(info.get("height", 0)),
            "bytes": int(info.get("size", 0) or 0),
        })
    return found


def yuv_pixel(r, g, b):
    yy = (77 * r + 150 * g + 29 * b) >> 8
    uu = 128 + ((-43 * r - 85 * g + 128 * b) >> 8)
    vv = 128 + ((128 * r - 107 * g - 21 * b) >> 8)
    return max(0, min(255, yy)), max(0, min(255, uu)), max(0, min(255, vv))


def to_yuy2(src, dest, width, height):
    with Image.open(src) as opened:
        image = opened.convert("RGB")
    scale = max(width / image.width, height / image.height)
    resized = image.resize(
        (max(width, int(round(image.width * scale))), max(height, int(round(image.height * scale)))),
        Image.Resampling.LANCZOS,
    )
    left = max(0, (resized.width - width) // 2)
    top = max(0, (resized.height - height) // 2)
    raw = resized.crop((left, top, left + width, top + height)).tobytes()
    out = bytearray(width * height * 2)
    for y in range(height):
        row = y * width * 3
        for x in range(0, width, 2):
            i = row + x * 3
            y0, u0, v0 = yuv_pixel(raw[i], raw[i + 1], raw[i + 2])
            y1, u1, v1 = yuv_pixel(raw[i + 3], raw[i + 4], raw[i + 5])
            o = (y * width + x) * 2
            out[o] = y0
            out[o + 1] = (u0 + u1) // 2
            out[o + 2] = y1
            out[o + 3] = (v0 + v1) // 2
    dest.write_bytes(out)


def download_image(item, width):
    thumb = thumbnail(item["url"], width)
    blob = None
    used = item["url"]
    if thumb:
        try:
            blob = get(thumb)
            used = thumb
        except Exception:
            blob = None
    if blob is None:
        if item["bytes"] and item["bytes"] > 40_000_000:
            raise RuntimeError("original too large and thumbnail failed")
        blob = get(item["url"])
        used = item["url"]
    temp = MEDIA / "_download.bin"
    temp.write_bytes(blob)
    return temp, used


def row(case_id, family, width, height, path, license_text, url, title, quality, tag):
    fields = [
        case_id, family, str(width), str(height), path, license_text, url, title,
        quality, PHOTO_SEED, "reencoded_real", tag, "false",
    ]
    return "\t".join(fields) + "\n"


def main():
    MEDIA.mkdir(parents=True, exist_ok=True)
    blocked = prior_text()
    seen = set()
    lines = [HEADER]
    accepted = 0
    four_k = 0
    for family, primary, fallback, tag in SEARCHES:
        if accepted >= TARGET:
            break
        chosen = None
        for query in (primary, fallback):
            try:
                hits = search(query)
            except Exception as exc:  # noqa: BLE001
                print(f"search failed {query}: {exc}", flush=True)
                time.sleep(3)
                continue
            for hit in hits:
                title = hit["title"].strip().lower()
                if title in seen or title in blocked:
                    continue
                if not any(stem in title for stem in KEYWORDS[family]):
                    continue
                chosen = hit
                break
            if chosen:
                break
            time.sleep(1.0)
        if not chosen:
            print(f"no image for {family}", flush=True)
            continue
        want_4k = four_k < MAX_4K and chosen["width"] >= 3200 and chosen["height"] >= 1800
        temp = MEDIA / "_download.bin"
        try:
            src, used = download_image(chosen, 3840 if want_4k else 1920)
            yuv_1080 = MEDIA / f"real_{family}_1920.yuv"
            to_yuy2(src, yuv_1080, 1920, 1080)
            wrote_4k = False
            if want_4k:
                yuv_4k = MEDIA / f"real_{family}_3840.yuv"
                to_yuy2(src, yuv_4k, 3840, 2160)
                wrote_4k = True
        except Exception as exc:  # noqa: BLE001
            print(f"skip {family}: {exc}", flush=True)
            continue
        finally:
            if temp.exists():
                temp.unlink()
        seen.add(chosen["title"].strip().lower())
        q1 = QUALITIES[accepted % len(QUALITIES)]
        q2 = QUALITIES[(accepted + 3) % len(QUALITIES)]
        license_text = chosen["license"].replace("\t", " ")
        title = chosen["title"].replace("\t", " ")
        lines.append(row(
            f"real_{family}_1920_{q1}", family, 1920, 1080,
            f"benchmarks/phase3af_media/real_{family}_1920.yuv",
            license_text, used, title, q1, tag))
        lines.append(row(
            f"real_{family}_1920_{q2}", family, 1920, 1080,
            f"benchmarks/phase3af_media/real_{family}_1920.yuv",
            license_text, used, title, q2, tag))
        if wrote_4k:
            q4 = "filmscan3" if four_k % 2 == 0 else "medium"
            lines.append(row(
                f"real_{family}_3840_{q4}", family, 3840, 2160,
                f"benchmarks/phase3af_media/real_{family}_3840.yuv",
                license_text, used, title, q4, tag))
            four_k += 1
        accepted += 1
        print(f"accepted {accepted}: {family} 4k={wrote_4k} {chosen['title']}", flush=True)
        time.sleep(1.2)
    if accepted < 30:
        raise SystemExit(f"only {accepted} photographs; need at least 30")
    out = MEDIA / "photos.tsv"
    out.write_text("".join(lines), encoding="utf-8")
    print(f"wrote {out} sources={accepted} four_k_sources={four_k} rows={len(lines) - 1}", flush=True)


if __name__ == "__main__":
    main()
