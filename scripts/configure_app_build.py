#!/usr/bin/env python3
"""Build-time app identity and optional icon generator for GitHub Actions.

AndroidEmu workflow identity + fixed guest geometry v1
"""
import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
CONFIG = BUILD / "app-customization.json"
ASSETS = BUILD / "AppAssets.xcassets"

BUNDLE_RE = re.compile(r"^[A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)+$")

def validate_name(value: str) -> str:
    value = value.strip()
    if not value or len(value) > 64:
        raise ValueError("App name must contain 1..64 characters")
    if any(ord(ch) < 32 or ord(ch) == 127 for ch in value):
        raise ValueError("App name must not contain control characters")
    return value

def validate_bundle_id(value: str) -> str:
    value = value.strip()
    if len(value) > 255 or not BUNDLE_RE.fullmatch(value):
        raise ValueError("Bundle ID must look like com.example.app")
    return value

def download_icon(url: str, destination: Path) -> None:
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or not parsed.netloc:
        raise ValueError("Icon URL must be HTTPS")
    request = urllib.request.Request(url, headers={"User-Agent": "AndroidEmu-CI/1"})
    limit = 20 * 1024 * 1024
    total = 0
    with urllib.request.urlopen(request, timeout=45) as response, destination.open("wb") as out:
        final = urllib.parse.urlparse(response.geturl())
        if final.scheme != "https":
            raise ValueError("Icon redirect must remain HTTPS")
        while True:
            chunk = response.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            if total > limit:
                raise ValueError("Icon download exceeds 20 MiB")
            out.write(chunk)
    if total == 0:
        raise ValueError("Icon download is empty")

def image_dimensions(path: Path) -> tuple[int, int]:
    output = subprocess.check_output(
        ["sips", "-g", "pixelWidth", "-g", "pixelHeight", str(path)],
        text=True,
        stderr=subprocess.STDOUT,
    )
    values = {}
    for line in output.splitlines():
        if ":" in line:
            key, value = line.split(":", 1)
            key = key.strip()
            value = value.strip()
            if value.isdigit():
                values[key] = int(value)
    try:
        return values["pixelWidth"], values["pixelHeight"]
    except KeyError as exc:
        raise ValueError("Unable to read icon dimensions with sips") from exc

def build_icon_catalog(url: str) -> None:
    if shutil.which("sips") is None:
        raise RuntimeError("Custom icons require macOS sips")
    with tempfile.TemporaryDirectory(prefix="androidemu-icon-") as folder:
        folder = Path(folder)
        source = folder / "source-icon"
        square = folder / "square.png"
        download_icon(url, source)
        width, height = image_dimensions(source)
        if min(width, height) < 1024:
            raise ValueError("Icon source must be at least 1024x1024 pixels")
        side = min(width, height)
        subprocess.run(
            ["sips", "-s", "format", "png",
             "--cropToHeightWidth", str(side), str(side),
             str(source), "--out", str(square)],
            check=True,
            stdout=subprocess.DEVNULL,
        )

        appicon = ASSETS / "AppIcon.appiconset"
        shutil.rmtree(ASSETS, ignore_errors=True)
        appicon.mkdir(parents=True)
        entries = [
            ("iphone", "20x20", "2x", 40),
            ("iphone", "20x20", "3x", 60),
            ("iphone", "29x29", "2x", 58),
            ("iphone", "29x29", "3x", 87),
            ("iphone", "40x40", "2x", 80),
            ("iphone", "40x40", "3x", 120),
            ("iphone", "60x60", "2x", 120),
            ("iphone", "60x60", "3x", 180),
            ("ipad", "20x20", "1x", 20),
            ("ipad", "20x20", "2x", 40),
            ("ipad", "29x29", "1x", 29),
            ("ipad", "29x29", "2x", 58),
            ("ipad", "40x40", "1x", 40),
            ("ipad", "40x40", "2x", 80),
            ("ipad", "76x76", "1x", 76),
            ("ipad", "76x76", "2x", 152),
            ("ipad", "83.5x83.5", "2x", 167),
            ("ios-marketing", "1024x1024", "1x", 1024),
        ]
        for pixels in sorted({entry[3] for entry in entries}):
            destination = appicon / f"icon_{pixels}.png"
            subprocess.run(
                ["sips", "-s", "format", "png",
                 "--resampleHeightWidth", str(pixels), str(pixels),
                 str(square), "--out", str(destination)],
                check=True,
                stdout=subprocess.DEVNULL,
            )
        contents = {
            "images": [
                {"filename": f"icon_{pixels}.png",
                 "idiom": idiom, "scale": scale, "size": size}
                for idiom, size, scale, pixels in entries
            ],
            "info": {"author": "xcode", "version": 1},
        }
        (appicon / "Contents.json").write_text(
            json.dumps(contents, indent=2) + "\n", encoding="utf-8"
        )
        (ASSETS / "Contents.json").write_text(
            json.dumps({"info": {"author": "xcode", "version": 1}}, indent=2) + "\n",
            encoding="utf-8",
        )

def configure(name: str, bundle_id: str, icon_url: str) -> None:
    name = validate_name(name)
    bundle_id = validate_bundle_id(bundle_id)
    icon_url = icon_url.strip()
    BUILD.mkdir(parents=True, exist_ok=True)
    if icon_url:
        build_icon_catalog(icon_url)
    else:
        shutil.rmtree(ASSETS, ignore_errors=True)
    CONFIG.write_text(
        json.dumps(
            {
                "app_name": name,
                "bundle_id": bundle_id,
                "custom_icon": bool(icon_url),
            },
            ensure_ascii=False,
            indent=2,
        ) + "\n",
        encoding="utf-8",
    )
    print(f"Configured app name: {name}")
    print(f"Configured bundle ID: {bundle_id}")
    print("Custom icon:", "yes" if icon_url else "no")

def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="AndroidEmu")
    parser.add_argument("--bundle-id", default="org.androidemu.app")
    parser.add_argument("--icon-url", default="")
    args = parser.parse_args()
    configure(args.name, args.bundle_id, args.icon_url)

if __name__ == "__main__":
    main()
