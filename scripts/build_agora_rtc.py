#!/usr/bin/env python3
"""
Build all Agora RTC firmware variants for all supported boards.
Extracts and renames merged binaries per the naming convention in
docs/agora-rtc-integration-skills.md §9.

Usage:
    source /path/to/esp-idf/export.sh
    python3 scripts/build_agora_rtc.py [--region sh2|sg3]
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
RELEASES_DIR = PROJECT_ROOT / "releases"
VERSION = "2.4.0"

# Each variant: (board_dir, variant_name, output_name_stem, region)
# output_name_stem: BOARD_TYPE 去掉前缀 "BOARD_TYPE_" 后全小写（§9 命名规则）
VARIANTS = [
    # zhengchen-1.54tft-ml307
    ("zhengchen-1.54tft-ml307", "zhengchen-1.54tft-ml307-agora-sh2", "zhengchen_1_54tft_ml307", "sh2"),
    ("zhengchen-1.54tft-ml307", "zhengchen-1.54tft-ml307-agora-sg3", "zhengchen_1_54tft_ml307", "sg3"),

    # m5stack-core-s3
    ("m5stack-core-s3", "m5stack-core-s3-agora-sh2", "m5stack_core_s3", "sh2"),
    ("m5stack-core-s3", "m5stack-core-s3-agora-sg3", "m5stack_core_s3", "sg3"),

    # sensecap-watcher
    ("sensecap-watcher", "sensecap-watcher-agora-sh2", "seeed_studio_sensecap_watcher", "sh2"),
    ("sensecap-watcher", "sensecap-watcher-agora-sg3", "seeed_studio_sensecap_watcher", "sg3"),

    # waveshare esp32-s3-touch-amoled-1.75 (1.75 variant)
    ("waveshare/esp32-s3-touch-amoled-1.75", "esp32-s3-touch-amoled-1.75-agora-sh2",
     "waveshare_esp32_s3_touch_amoled_1_75", "sh2"),
    ("waveshare/esp32-s3-touch-amoled-1.75", "esp32-s3-touch-amoled-1.75-agora-sg3",
     "waveshare_esp32_s3_touch_amoled_1_75", "sg3"),

    # waveshare esp32-s3-touch-amoled-1.75 (1.75c variant)
    ("waveshare/esp32-s3-touch-amoled-1.75", "esp32-s3-touch-amoled-1.75c-agora-sh2",
     "waveshare_esp32_s3_touch_amoled_1_75c", "sh2"),
    ("waveshare/esp32-s3-touch-amoled-1.75", "esp32-s3-touch-amoled-1.75c-agora-sg3",
     "waveshare_esp32_s3_touch_amoled_1_75c", "sg3"),

    # zhengchen-1.54tft-wifi
    ("zhengchen-1.54tft-wifi", "zhengchen-1.54tft-wifi-agora-sh2", "zhengchen_1_54tft_wifi", "sh2"),
    ("zhengchen-1.54tft-wifi", "zhengchen-1.54tft-wifi-agora-sg3", "zhengchen_1_54tft_wifi", "sg3"),

    # esp-vocat
    ("esp-vocat", "esp-vocat-agora-sh2", "esp_vocat", "sh2"),
    ("esp-vocat", "esp-vocat-agora-sg3", "esp_vocat", "sg3"),
]


def run_release(board_dir: str, variant_name: str) -> bool:
    """Run release.py for one variant."""
    cmd = [
        sys.executable, str(PROJECT_ROOT / "scripts" / "release.py"),
        board_dir,
        "--name", variant_name,
    ]
    print(f">>> {' '.join(cmd)}")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT)
    return result.returncode == 0


def build_variant(board_dir: str, variant_name: str, output_stem: str, region: str) -> bool:
    """Build one variant and extract/rename the merged binary."""
    output_name = f"{output_stem}_{region}.bin"
    output_path = RELEASES_DIR / output_name

    if output_path.exists():
        print(f"  [SKIP] {output_name} already exists")
        return True

    print(f"\n{'=' * 70}")
    print(f"  Building: {variant_name}")
    print(f"  Output:   {output_name}")
    print(f"{'=' * 70}")

    # Step 1: Build with release.py
    if not run_release(board_dir, variant_name):
        print(f"  [FAIL] Build failed for {variant_name}")
        return False

    # Step 2: Locate the zip file
    # release.py names it: v{version}_{full_name}.zip
    # full_name can have a manufacturer prefix for subdirectory boards
    # The zip is at: releases/v{version}_{full_name}.zip
    zip_files = list(RELEASES_DIR.glob(f"v{VERSION}_*{variant_name.replace('/', '-')}.zip"))

    if not zip_files:
        # Try broader search
        zip_files = list(RELEASES_DIR.glob(f"v{VERSION}_*{variant_name.split('/')[-1]}.zip"))

    if not zip_files:
        print(f"  [FAIL] No zip file found for {variant_name}")
        return False

    zip_path = zip_files[0]
    print(f"  Found zip: {zip_path.name}")

    # Step 3: Extract merged-binary.bin from zip
    with zipfile.ZipFile(zip_path) as zf:
        if "merged-binary.bin" not in zf.namelist():
            print(f"  [FAIL] merged-binary.bin not found in {zip_path.name}")
            return False

        zf.extract("merged-binary.bin", RELEASES_DIR)

    # Step 4: Rename
    extracted = RELEASES_DIR / "merged-binary.bin"
    shutil.move(extracted, output_path)

    size_mb = output_path.stat().st_size / (1024 * 1024)
    print(f"  [OK] {output_name} ({size_mb:.1f} MB)")
    return True


def main():
    parser = argparse.ArgumentParser(description="Build and package all Agora RTC firmware variants")
    parser.add_argument("--region", choices=["sh2", "sg3"],
                        help="Build only one region (default: both)")
    parser.add_argument("--board", help="Build only one board directory")
    args = parser.parse_args()

    if not os.environ.get("IDF_PATH"):
        print("[ERROR] ESP-IDF not sourced. Run: source /path/to/esp-idf/export.sh")
        sys.exit(1)

    # Clean releases directory
    if RELEASES_DIR.exists():
        for f in RELEASES_DIR.iterdir():
            if f.is_file():
                f.unlink()
    print(f"Cleaned {RELEASES_DIR}")

    # Filter variants
    variants = VARIANTS
    if args.region:
        variants = [v for v in variants if v[3] == args.region]
    if args.board:
        variants = [v for v in variants if v[0] == args.board]

    print(f"ESP-IDF: {os.environ['IDF_PATH']}")
    print(f"Variants to build: {len(variants)}")
    print()

    results = []
    for board_dir, variant_name, output_stem, region in variants:
        ok = build_variant(board_dir, variant_name, output_stem, region)
        results.append((variant_name, ok))

    # Summary
    print(f"\n{'=' * 70}")
    print("  BUILD SUMMARY")
    print(f"{'=' * 70}")
    for name, ok in results:
        print(f"  [{'OK' if ok else 'FAIL'}] {name}")
    success = sum(1 for _, ok in results if ok)
    print(f"\n  {success}/{len(results)} succeeded")
    print(f"  Output: {RELEASES_DIR}/")

    for f in sorted(RELEASES_DIR.glob("*.bin")):
        size_mb = f.stat().st_size / (1024 * 1024)
        print(f"    {f.name} ({size_mb:.1f} MB)")


if __name__ == "__main__":
    main()
