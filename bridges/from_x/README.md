# `quack_from_x` — framework-import bridges

Point quackapi at an existing web-framework source tree and get **route + model IR
rows** via native C++ table functions that wrap `sitting_duck` (tree-sitter AST).

## Native table functions (shipped in the extension)

```sql
LOAD quackapi;

-- Routes: method, path, handler_name, file, start_line, evidence
SELECT * FROM quack_from_routes('fastapi', '/path/to/app');
SELECT * FROM quack_from_routes('rails', '/path/to/rails-app');
SELECT * FROM quack_from_routes('express', '/path/to/express-app');
SELECT * FROM quack_from_routes('gin', '/path/to/gin-app');

-- Models: model_name, field_name, field_type, is_required, is_optional,
--         has_default, default_expr, file, field_line
SELECT * FROM quack_from_models('fastapi', '/path/to/app');
SELECT * FROM quack_from_models('rails', '/path/to/rails-app');
SELECT * FROM quack_from_models('express', '/path/to/express-app');
SELECT * FROM quack_from_models('gin', '/path/to/gin-app');
```

Each function **auto-`INSTALL`/`LOAD`s `sitting_duck` FROM community** on first use
(no-op if already loaded). Pass a project root directory, a file, or a glob.

## Source of truth

Extraction SQL lives under `src/sql/`:

```
src/sql/quack_from_fastapi_routes.sql
src/sql/quack_from_fastapi_models.sql
src/sql/quack_from_rails_routes.sql
src/sql/quack_from_rails_models.sql
src/sql/quack_from_express_routes.sql
src/sql/quack_from_express_models.sql
src/sql/quack_from_gin_routes.sql
src/sql/quack_from_gin_models.sql
```

These are embedded into the binary as raw-string constants
(`src/include/quackapi_from_x_sql.hpp`, regenerated via
`python3 scripts/embed_from_x_sql.py`). Run
`python3 test/test_from_x_sql_embed.py` to verify all eight embedded queries
match their source files byte for byte.

## Layout

```
bridges/from_x/
├── README.md
├── fixtures/
│   ├── fastapi_mini/   committed FastAPI fixture
│   ├── rails_mini/     committed Rails fixture
│   ├── express_mini/   committed Express fixture
│   └── gin_mini/       committed Gin fixture
└── docs/                               original proof reports
    ├── fromfast.md
    ├── rails_bridge.md
    ├── pydantic_bridge.md
    └── handler_bridge.md

src/quackapi_from_x.cpp                 table function implementations
src/sql/*.sql                           extraction SQL (source of truth)
test/sql/quackapi_from_x_*.test         sqllogictests
```

## What translates automatically

1. **Routing** — decorator/DSL syntax → `(method, path, handler_name)` IR →
   `CREATE ROUTE … AS SELECT …`
2. **Request-body validation** — Pydantic / class-validator / Rails validates+
   strong-params / Go struct tags → field IR → `BODY SCHEMA` JSON Schema

## Escape hatch

**Imperative handler bodies are never transpiled.** Register non-declarative
logic as macros/tables/queues and call them from the route SQL.

## Tests

```bash
./build/release/test/unittest 'test/sql/quackapi_from_x*'
```
