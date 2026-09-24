#!/usr/bin/env python3
"""Упаковка прошивки ESP32 (JScaner.bin + storage.bin) в update.zip для OTA."""
import os
import sys
import zipfile

FIRMWARE = "JScaner.bin"
STORAGE = "storage.bin"
ZIP_NAME = "update.zip"


def main() -> int:
    script_dir = os.path.dirname(os.path.abspath(__file__))

    build_dir = None
    if len(sys.argv) > 1:
        build_dir = sys.argv[1]
    elif os.path.isdir(os.path.join(os.getcwd(), "build")):
        build_dir = os.path.join(os.getcwd(), "build")
    elif os.path.isdir(os.path.join(script_dir, "build")):
        build_dir = os.path.join(script_dir, "build")
    else:
        print("ERROR: папка 'build' не найдена", file=sys.stderr)
        return 1

    src_fw = os.path.join(build_dir, FIRMWARE)
    src_fs = os.path.join(build_dir, STORAGE)

    missing = [p for p in (src_fw, src_fs) if not os.path.isfile(p)]
    if missing:
        for p in missing:
            print(f"ERROR: файл не найден: {p}", file=sys.stderr)
        return 1

    out_zip = os.path.join(os.path.dirname(build_dir), ZIP_NAME)

    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.write(src_fw, arcname=FIRMWARE)
        zf.write(src_fs, arcname=STORAGE)

    print(f"OK: создан архив {out_zip}")
    return 0


if __name__ == "__main__":
    sys.exit(main())