# Copyright (c) 2026 HyperCine Contributors.
# Dual-licensed under the MIT License or Apache License, Version 2.0.
# See LICENSE-MIT and LICENSE-APACHE for details.
"""Download openly licensed photographs for Phase 3A-D validation.

Writes YUV422 raw frames and a provenance manifest. Hold-out titles are recorded
and are not downloaded here. Re-encoded frames are not native camera CineForm.
"""
import json
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MEDIA = ROOT / "benchmarks" / "phase3ad_media"
API = "https://commons.wikimedia.org/w/api.php"
UA = "HyperCinePhase3AD/1.0 (local codec research; contact: none)"

# Predeclared subject searches. The first accepted file in each of the first
# eight searches is validation imagery. The second accepted file is recorded as
# a future hold-out title and is not fetched.
SEARCHES = [
    ("architecture", "incategory:CC-Zero architecture building"),
    ("foliage", "incategory:CC-Zero forest trees"),
    ("grass", "incategory:CC-Zero grass meadow"),
    ("water", "incategory:CC-Zero waterfall"),
    ("clouds", "incategory:CC-Zero clouds sky"),
    ("skin", "incategory:CC-Zero portrait face"),
    ("textile", "incategory:CC-Zero fabric textile"),
    ("city", "incategory:CC-Zero city night"),
    ("brick", "incategory:CC-Zero brick wall"),
    ("ocean", "incategory:CC-Zero ocean waves"),
    ("handwriting", "incategory:CC-Zero handwriting"),
    ("marble", "incategory:CC-Zero marble texture"),
    ("rain", "incategory:CC-Zero rain drops"),
    ("circuit", "incategory:CC-Zero circuit board"),
    ("flower", "incategory:CC-Zero flower macro"),
    ("mountain", "incategory:CC-Zero mountain landscape"),
]


def get(url):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=60) as response:
        return response.read()


def thumbnail(url):
    base = url.split("?", 1)[0]
    marker = "/wikipedia/commons/"
    pos = base.find(marker)
    if pos < 0 or "/thumb/" in base:
        return None
    rest = base[pos + len(marker):]
    name = rest.rsplit("/", 1)[-1]
    thumb_name = name + ".jpg" if name.lower().endswith((".tif", ".tiff")) else name
    return base[:pos] + "/wikipedia/commons/thumb/" + rest + "/1920px-" + thumb_name


def license_name(meta):
    short = meta.get("LicenseShortName", {}).get("value", "")
    terms = meta.get("UsageTerms", {}).get("value", "")
    return f"{short} {terms}".strip()


def acceptable(name):
    text = name.lower()
    if any(flag in text for flag in ("nc", "nd", "noncommercial", "no derivative")):
        return False
    return any(flag in text for flag in ("cc0", "cc-zero", "public domain", "cc by", "cc-by"))


def search(query):
    params = {
        "action": "query",
        "generator": "search",
        "gsrsearch": query,
        "gsrnamespace": "6",
        "gsrlimit": "8",
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
            "width": info.get("width", 0),
            "height": info.get("height", 0),
        })
    return found


def to_yuy2(src, dest, width, height):
    with Image.open(src) as opened:
        image = opened.convert("RGB")
    scale = max(width / image.width, height / image.height)
    resized = image.resize((max(width, int(image.width * scale)), max(height, int(image.height * scale))), Image.Resampling.LANCZOS)
    left = max(0, (resized.width - width) // 2)
    top = max(0, (resized.height - height) // 2)
    crop = resized.crop((left, top, left + width, top + height))
    raw = crop.tobytes()
    out = bytearray(width * height * 2)
    for y in range(height):
        for x in range(0, width, 2):
            i = (y * width + x) * 3
            r0, g0, b0 = raw[i], raw[i + 1], raw[i + 2]
            r1, g1, b1 = raw[i + 3], raw[i + 4], raw[i + 5]
            def yuv(r, g, b):
                yy = (77 * r + 150 * g + 29 * b) >> 8
                uu = 128 + ((-43 * r - 85 * g + 128 * b) >> 8)
                vv = 128 + ((128 * r - 107 * g - 21 * b) >> 8)
                return max(0, min(255, yy)), max(0, min(255, uu)), max(0, min(255, vv))
            y0, u0, v0 = yuv(r0, g0, b0)
            y1, u1, v1 = yuv(r1, g1, b1)
            o = (y * width + x) * 2
            out[o] = y0
            out[o + 1] = (u0 + u1) // 2
            out[o + 2] = y1
            out[o + 3] = (v0 + v1) // 2
    dest.write_bytes(out)


def download_recorded_holdout():
    """Download titles recorded before any oracle measurement. Does not touch validation."""
    titles = MEDIA / "holdout_titles.txt"
    manifest = MEDIA / "holdout_manifest.tsv"
    if not titles.exists():
        raise SystemExit("hold-out titles were not recorded")
    header = "id\tsplit\tfamily\twidth\theight\tpath\tlicense\tsource_url\tsource_identity\n"
    existing = manifest.read_text(encoding="utf-8") if manifest.exists() else header
    rows = [line for line in existing.splitlines()[1:] if line.strip()]
    have = {line.split("\t", 1)[0] for line in rows}
    for line in titles.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) < 4:
            print("skip malformed hold-out title")
            continue
        family, title, license, url = parts[0], parts[1], parts[2], parts[3]
        if not acceptable(license):
            print("reject hold-out license", title)
            continue
        hid = f"photo_holdout_{family}"
        dest = MEDIA / f"{hid}.yuv"
        src = MEDIA / f"{hid}.download"
        if hid in have and dest.exists():
            continue
        time.sleep(4)
        used = None
        for candidate in (url, thumbnail(url)):
            if not candidate:
                continue
            try:
                src.write_bytes(get(candidate))
                to_yuy2(src, dest, 1920, 1080)
                src.unlink(missing_ok=True)
                used = candidate
                break
            except Exception as error:
                print(f"hold-out download failed {hid}: {error}")
                try:
                    src.unlink(missing_ok=True)
                except OSError:
                    pass
        if used is None:
            continue
        identity = title if used == url else title + " [1920px thumbnail; original rate-limited]"
        rows.append("\t".join([
            hid, "holdout", f"holdout_{family}", "1920", "1080",
            f"benchmarks/phase3ad_media/{hid}.yuv", license, url, identity,
        ]))
        print("hold-out", hid, "thumb" if used != url else "original", license)
    if not rows:
        raise SystemExit("no hold-out images were downloaded")
    manifest.write_text(
        "id\tsplit\tfamily\twidth\theight\tpath\tlicense\tsource_url\tsource_identity\n" + "\n".join(rows) + "\n",
        encoding="utf-8")
    print(f"hold-out images: {len(rows)}")


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--holdout":
        download_recorded_holdout()
        return
    MEDIA.mkdir(parents=True, exist_ok=True)
    manifest = MEDIA / "validation_manifest.tsv"
    titles = MEDIA / "holdout_titles.txt"
    rows = []
    holdout = []
    for index, (family, query) in enumerate(SEARCHES):
        try:
            hits = search(query)
        except Exception as error:
            print(f"search failed {family}: {error}")
            continue
        if index < 8 and hits:
            item = hits[0]
            name = f"photo_{family}.yuv"
            jpg = MEDIA / f"photo_{family}.download"
            try:
                jpg.write_bytes(get(item["url"]))
                to_yuy2(jpg, MEDIA / name, 1920, 1080)
                jpg.unlink(missing_ok=True)
                rows.append("\t".join([
                    f"photo_{family}", "validation", family, "1920", "1080",
                    f"benchmarks/phase3ad_media/{name}", item["license"], item["url"], item["title"],
                ]))
                print("validation", item["title"], item["license"])
            except Exception as error:
                print(f"download failed {family}: {error}")
                try:
                    jpg.unlink(missing_ok=True)
                except OSError:
                    pass
        if len(hits) > 1:
            holdout.append(f"{family}\t{hits[1]['title']}\t{hits[1]['license']}\t{hits[1]['url']}")
        elif len(hits) == 1 and index >= 8:
            holdout.append(f"{family}\t{hits[0]['title']}\t{hits[0]['license']}\t{hits[0]['url']}")
    if rows:
        manifest.write_text(
            "id\tsplit\tfamily\twidth\theight\tpath\tlicense\tsource_url\tsource_identity\n" + "\n".join(rows) + "\n",
            encoding="utf-8")
    if holdout and not titles.exists():
        titles.write_text("\n".join(holdout) + "\n", encoding="utf-8")
        print(f"recorded {len(holdout)} hold-out titles without downloading them")
    print(f"validation images: {len(rows)}")


if __name__ == "__main__":
    main()
