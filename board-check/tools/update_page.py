#!/usr/bin/env python3
"""Copy the built check program into docs/board-check/firmware/ and write
each file's SHA-256 into the page, which checks them before writing.

Run from the repository root after `pio run -d board-check`.
"""
import hashlib
import os
import re
import shutil
import sys

BUILD = "board-check/.pio/build/board_check_esp32s3"
FILES = {
    "bootloader.bin": os.path.join(BUILD, "bootloader.bin"),
    "partitions.bin": os.path.join(BUILD, "partitions.bin"),
    "boot_app0.bin": os.path.expanduser(
        "~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"),
    "board_check_esp32s3.bin": os.path.join(BUILD, "firmware.bin"),
}
OUT = "docs/board-check/firmware"
PAGE = "docs/board-check/index.html"


def main():
    os.makedirs(OUT, exist_ok=True)
    page = open(PAGE).read()
    for name, src in FILES.items():
        dst = os.path.join(OUT, name)
        shutil.copyfile(src, dst)
        sha = hashlib.sha256(open(dst, "rb").read()).hexdigest()
        page, n = re.subn(r"(name: '%s',\s+address: 0x[0-9a-f]+,\s+sha256: ')[^']*(')" % re.escape(name),
                          r"\g<1>%s\g<2>" % sha, page)
        if n != 1:
            sys.exit("%s: not found in the page" % name)
        print(sha, name)
    open(PAGE, "w").write(page)


if __name__ == "__main__":
    main()
