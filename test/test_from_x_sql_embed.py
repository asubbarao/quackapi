#!/usr/bin/env python3
"""Verify every embedded framework importer query matches its SQL source."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src/include/quackapi_from_x_sql.hpp"
SQL_DIR = ROOT / "src/sql"
ENTRIES = {
    "fastapi_routes": "quack_from_fastapi_routes.sql",
    "fastapi_models": "quack_from_fastapi_models.sql",
    "rails_routes": "quack_from_rails_routes.sql",
    "rails_models": "quack_from_rails_models.sql",
    "express_routes": "quack_from_express_routes.sql",
    "express_models": "quack_from_express_models.sql",
    "gin_routes": "quack_from_gin_routes.sql",
    "gin_models": "quack_from_gin_models.sql",
}


def main() -> None:
    header = HEADER.read_text()
    embedded = dict(
        re.findall(
            r'static constexpr const char \*k_(\w+) =\s*R"__QUACKAPI_SQL__\((.*?)\)__QUACKAPI_SQL__";',
            header,
            re.DOTALL,
        )
    )
    if set(embedded) != set(ENTRIES):
        raise AssertionError(f"embedded SQL entries differ: expected {sorted(ENTRIES)}, got {sorted(embedded)}")

    for name, filename in ENTRIES.items():
        source = (SQL_DIR / filename).read_text()
        if embedded[name] != source:
            raise AssertionError(f"embedded SQL drift: {filename} != k_{name}")
    print(f"verified {len(ENTRIES)} embedded importer SQL files")


if __name__ == "__main__":
    main()
