#!/usr/bin/env python3
"""Purpose: Assemble legacy-node and board-independent ESP32-S3 updater bundles.

Summary: Generate both provisioning tables with the pinned IDF tool and package
one bootloader/updater pair. Release versions use the same c-cpp-esp32-v* tag as
the legacy node zip; local builds use git describe for an identifiable snapshot.
Owns: The provisioning manifest and the universal bundle's six-file contract.
Not here: Per-board cancfg contents (the provisioning tool supplies those).
Errors: Missing inputs, oversized images and failed partition generation abort.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import zipfile

TAG_PREFIX = "c-cpp-esp32-"
FACTORY_SIZE = 0x60000
BOOTLOADER_SIZE = 0x8000
OTADATA_SIZE = 0x2000


def version_from_tag(repo):
    tag = os.environ.get("GITHUB_REF_NAME", "")
    if not tag.startswith(TAG_PREFIX + "v"):
        tag = subprocess.check_output(
            ["git", "-C", str(repo), "describe", "--tags", "--match", TAG_PREFIX + "v*", "--dirty"],
            text=True,
        ).strip()
    return tag[len(TAG_PREFIX):]


def build(repo, idf_path):
    project = repo / "node_updater"
    build_dir = project / "build"
    version = version_from_tag(repo)
    tables = {
        "node-4MB": ("partitions.csv", "4MB", "0x20000", "0x80000"),
        "idu-16MB": ("partitions_idu_16MB.csv", "16MB", "0xfa0000", "0x20000"),
    }
    layouts = {}
    for name, (csv, flash_size, factory, ota_0) in tables.items():
        table = f"partition-table-{name}.bin"
        subprocess.run([
            sys.executable, str(idf_path / "components/partition_table/gen_esp32part.py"),
            "--flash-size", flash_size, str(project / csv), str(build_dir / table),
        ], check=True)
        layouts[name] = dict(table=table, flash_size=flash_size, bootloader="0x0",
                             table_offset="0x8000", otadata="0xe000", cancfg="0x11000",
                             factory=factory, ota_0=ota_0)

    files = {"bootloader.bin": build_dir / "bootloader/bootloader.bin",
             "can_updater.bin": build_dir / "can_updater.bin",
             "ota_data_initial.bin": build_dir / "ota_data_initial.bin"}
    for filename, limit in (("bootloader.bin", BOOTLOADER_SIZE), ("can_updater.bin", FACTORY_SIZE)):
        size = files[filename].stat().st_size
        if not 0 < size < limit:
            raise ValueError(f"{filename}: {size} bytes must fit below {limit:#x}")
    if files["ota_data_initial.bin"].read_bytes() != b"\xff" * OTADATA_SIZE:
        raise ValueError("ota_data_initial.bin must be exactly 8 KB of erased bytes")
    files.update({layout["table"]: build_dir / layout["table"] for layout in layouts.values()})
    manifest = dict(format=1, version=version[1:] if version.startswith("v") else version, chip="esp32s3",
                    flash_mode="dio", flash_freq="80m",
                    files=dict(bootloader="bootloader.bin", updater="can_updater.bin",
                               otadata="ota_data_initial.bin"), layouts=layouts)
    bundle = build_dir / f"esp32s3-can-updater-{version}.zip"
    with zipfile.ZipFile(bundle, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in files.items():
            archive.write(path, name)
        archive.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")

    # Keep the original node archive's relative paths and flashing metadata.
    node_zip = build_dir / f"esp32s3-node-updater-{version}.zip"
    with zipfile.ZipFile(node_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name in ("bootloader/bootloader.bin", "partition_table/partition-table.bin",
                     "ota_data_initial.bin", "can_updater.bin", "flash_args", "flasher_args.json"):
            archive.write(build_dir / name, name)
    print(f"Updater bundles: {node_zip}\n                 {bundle}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--idf-path", type=Path, required=True)
    args = parser.parse_args()
    try:
        build(args.repo, args.idf_path)
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"updater_bundle: {exc}\n")


if __name__ == "__main__":
    main()
