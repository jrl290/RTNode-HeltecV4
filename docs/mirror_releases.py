#!/usr/bin/env python3
"""Mirror RTNode GitHub release binaries into docs/firmware/<tag>/.

Why: GitHub's release-download URLs do not expose CORS headers, so the
browser-based web flasher (docs/index.html) cannot fetch them directly from
jrl290.github.io. By mirroring the .bin assets under GitHub Pages we get a
same-origin URL the browser is allowed to fetch.

Usage:
    python3 docs/mirror_releases.py                # mirror all known releases
    python3 docs/mirror_releases.py --max 5        # only the most recent 5
    python3 docs/mirror_releases.py --tag v1.0.28  # one specific tag

Outputs:
    docs/firmware/<tag>/<asset>.bin                # mirrored binaries
    docs/firmware/versions.json                    # manifest consumed by index.html

Convention:
    All published firmware releases are treated as Beta unless explicitly
    promoted to a different stability level elsewhere.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request
import urllib.error
from pathlib import Path

REPO = "jrl290/RTNode-HeltecV4"
API = f"https://api.github.com/repos/{REPO}/releases"
ASSET_NAMES = (
    "rtnode_heltec_v3.bin",
    "rtnode_heltec_v3_merged.bin",
    "rtnode_heltec_v4.bin",
    "rtnode_heltec_v4_merged.bin",
    "rtnode_heltec_v4_r8.bin",
    "rtnode_heltec_v4_r8_merged.bin",
)
EXCLUDED_TAGS = {
    "v1.0.31",
    "v1.0.32",
}
DEFAULT_STABILITY = "beta"

DOCS_DIR = Path(__file__).resolve().parent
FW_DIR = DOCS_DIR / "firmware"
MANIFEST = FW_DIR / "versions.json"


def load_manifest() -> list[dict]:
    if not MANIFEST.exists():
        return []

    try:
        data = json.loads(MANIFEST.read_text())
    except (json.JSONDecodeError, OSError):
        return []

    return [entry for entry in data if isinstance(entry, dict) and entry.get("tag")]


def sort_manifest(entries: list[dict]) -> list[dict]:
    return sorted(
        entries,
        key=lambda entry: (entry.get("published_at") or "", entry.get("tag") or ""),
        reverse=True,
    )


def merge_manifest(existing: list[dict], updates: list[dict]) -> list[dict]:
    by_tag = {entry["tag"]: entry for entry in existing if entry.get("tag")}
    for entry in updates:
        by_tag[entry["tag"]] = entry
    return sort_manifest(list(by_tag.values()))


def gh_get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "rtnode-mirror-script",
    })
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req) as r:
        return r.read()


def list_releases() -> list[dict]:
    out: list[dict] = []
    page = 1
    while True:
        data = json.loads(gh_get(f"{API}?per_page=100&page={page}"))
        if not data:
            break
        out.extend(data)
        if len(data) < 100:
            break
        page += 1
    return out


def download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".part")
    req = urllib.request.Request(url, headers={"User-Agent": "rtnode-mirror-script"})
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(64 * 1024)
            if not chunk:
                break
            f.write(chunk)
    tmp.replace(dest)


def mirror(releases: list[dict]) -> list[dict]:
    manifest: list[dict] = []
    for rel in releases:
        tag = rel["tag_name"]
        if tag in EXCLUDED_TAGS:
            print(f"  - {tag}: excluded, skipping")
            continue

        assets = {a["name"]: a for a in rel.get("assets", [])}
        present = [n for n in ASSET_NAMES if n in assets]
        if not present:
            print(f"  - {tag}: no recognised .bin assets, skipping")
            continue

        tag_dir = FW_DIR / tag
        for name in present:
            dest = tag_dir / name
            if dest.exists() and dest.stat().st_size == assets[name]["size"]:
                print(f"  = {tag}/{name} ({dest.stat().st_size} bytes, cached)")
                continue
            url = assets[name]["browser_download_url"]
            print(f"  + {tag}/{name} <- {url}")
            try:
                download(url, dest)
            except urllib.error.HTTPError as e:
                print(f"    ! HTTP {e.code} fetching {url}")
                continue

        manifest.append({
            "tag": tag,
            "name": rel.get("name") or tag,
            "stability": DEFAULT_STABILITY,
            "prerelease": rel.get("prerelease", False),
            "published_at": rel.get("published_at"),
            "assets": sorted(present),
        })
    return manifest


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--max", type=int, default=0, help="only mirror the N most recent releases (0 = all)")
    p.add_argument("--tag", action="append", default=[], help="mirror only this tag (may be repeated)")
    args = p.parse_args()

    print(f"Fetching release list for {REPO}...")
    releases = list_releases()
    if args.tag:
        releases = [r for r in releases if r["tag_name"] in args.tag]
    if args.max > 0:
        releases = releases[: args.max]
    print(f"Mirroring {len(releases)} release(s) into {FW_DIR}")

    manifest = mirror(releases)
    if args.tag:
        manifest = merge_manifest(load_manifest(), manifest)
    else:
        manifest = sort_manifest(manifest)

    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Wrote {MANIFEST} ({len(manifest)} versions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
