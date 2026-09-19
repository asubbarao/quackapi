#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--destination", type=Path, required=True)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text())
    repository = manifest["repository"].removeprefix("https://github.com/")
    commit = manifest["commit"]
    application_root = manifest["application_root"]
    target_root = args.destination / application_root

    for relative, expected_sha256 in manifest["files"].items():
        target = target_root / relative
        url = (
            f"https://raw.githubusercontent.com/{repository}/{commit}/"
            f"{application_root}/{relative}"
        )
        data = target.read_bytes() if target.exists() else urllib.request.urlopen(url).read()
        actual_sha256 = hashlib.sha256(data).hexdigest()
        if actual_sha256 != expected_sha256:
            raise SystemExit(
                f"checksum mismatch for {relative}: {actual_sha256} != {expected_sha256}"
            )
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)

    print(target_root)


if __name__ == "__main__":
    main()
