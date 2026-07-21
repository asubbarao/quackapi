#!/usr/bin/env python3
"""Regenerate src/include/quackapi_from_x_sql.hpp from src/sql/*.sql."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SQL_DIR = ROOT / "src" / "sql"
OUT = ROOT / "src" / "include" / "quackapi_from_x_sql.hpp"

ENTRIES = [
    ("fastapi", "routes", "quack_from_fastapi_routes.sql"),
    ("fastapi", "models", "quack_from_fastapi_models.sql"),
    ("rails", "routes", "quack_from_rails_routes.sql"),
    ("rails", "models", "quack_from_rails_models.sql"),
    ("express", "routes", "quack_from_express_routes.sql"),
    ("express", "models", "quack_from_express_models.sql"),
    ("gin", "routes", "quack_from_gin_routes.sql"),
    ("gin", "models", "quack_from_gin_models.sql"),
]
DELIM = "__QUACKAPI_SQL__"


def main() -> None:
    parts = [
        "#pragma once",
        "// AUTO-GENERATED from src/sql/*.sql — do not edit by hand.",
        "// Regenerate: python3 scripts/embed_from_x_sql.py",
        "// Source of truth is src/sql/; a sqllogictest asserts byte-equality.",
        "",
        "#include <string>",
        "#include <utility>",
        "#include <vector>",
        "",
        "namespace duckdb {",
        "namespace quackapi_from_x_sql {",
        "",
    ]
    for fw, kind, fname in ENTRIES:
        text = (SQL_DIR / fname).read_text()
        if DELIM in text or f"){DELIM}" in text:
            raise SystemExit(f"delimiter collision in {fname}")
        var = f"k_{fw}_{kind}"
        parts.append(f"// src/sql/{fname}")
        parts.append(f'static constexpr const char *{var} = R"{DELIM}({text}){DELIM}";')
        parts.append("")
    parts += [
        "struct EmbeddedSql {",
        "\tconst char *framework;",
        '\tconst char *kind; // "routes" | "models"',
        "\tconst char *relpath; // path under repo root",
        "\tconst char *sql;",
        "};",
        "",
        "inline const std::vector<EmbeddedSql> &All() {",
        "\tstatic const std::vector<EmbeddedSql> k = {",
    ]
    for fw, kind, fname in ENTRIES:
        var = f"k_{fw}_{kind}"
        parts.append(f'\t\t{{"{fw}", "{kind}", "src/sql/{fname}", {var}}},')
    parts += [
        "\t};",
        "\treturn k;",
        "}",
        "",
        "inline const char *Lookup(const char *framework, const char *kind) {",
        "\tfor (const auto &e : All()) {",
        "\t\tif (std::string(e.framework) == framework && std::string(e.kind) == kind) {",
        "\t\t\treturn e.sql;",
        "\t\t}",
        "\t}",
        "\treturn nullptr;",
        "}",
        "",
        "} // namespace quackapi_from_x_sql",
        "} // namespace duckdb",
        "",
    ]
    OUT.write_text("\n".join(parts))
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
