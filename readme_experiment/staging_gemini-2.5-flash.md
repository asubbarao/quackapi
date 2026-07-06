# quackapi: Beyond FastAPI. Beyond the Database.

**quackapi** transforms DuckDB into a fully-fledged, high-performance web framework. It is not a wrapper, nor a binding. It is the framework layer—routing, validation, serialization, and OpenAPI generation—re-derived from first principles, where every component is either a SQL query or a JIT-compiled C extension running directly within the DuckDB process.

The ambition is clear: to offer an alternative so compelling that you reconsider your entire web stack.

## The Pitch: A DuckDB-Native Web Framework

Why choose **quackapi** over conventional web frameworks like FastAPI?
Because the core of a web framework—routing, request parsing, data validation, and response serialization—is fundamentally a data transformation problem. And a database is the ultimate data transformation engine.

| FastAPI / Pydantic Concept             | quackapi Implementation                                                                 |
| :------------------------------------- | :-------------------------------------------------------------------------------------- |
| `@app.get("/users/{id}")` decorator    | A **row** in a `routes` table (`register_route(...)`)                                   |
| Path/query/body parsing                | Segment-array structural match — **no regex**                                           |
| `BaseModel` field types + validators   | `TRY_CAST` + a `param_schema` constraint table                                          |
| `ValidationError` → 422 `detail[]`     | Aggregates every failure into FastAPI's exact JSON shape (`type`, `loc`, `msg`, `input`) |
| `response_model` serialization         | `to_json()` / `json_group_array()`                                                      |
| `/openapi.json` from type hints        | A **`SELECT`** over `routes` + `param_schema`                                           |
| `/docs` Swagger UI                     | A route whose body is the Swagger HTML                                                  |
| `BackgroundTasks`                      | A detached C thread internal execution channel                                          |
| Concurrent request handling            | A 16-thread C `accept()` loop, one DuckDB connection each                               |

## Quick Start

Experience **quackapi** locally:

```bash
# Clone the repository
git clone --recurse-submodules https://github.com/aloksubbarao/quackapi.git
cd quackapi

# Build the C++ extension (see ext-cpp/docs/README.md for full details)
cd ext-cpp
make
cd ..

# Launch the server (boots on http://127.0.0.1:18099)
# Requires 'duckdb' CLI on your PATH, built with the quackapi extension loaded.
./run.sh
```

Then, interact with your new DuckDB-powered API:

```bash
# Get a user by ID
curl http://localhost:18099/users/1
# => {"id":1,"name":"alice","age":30}

# Input validation matching FastAPI's 422
curl http://localhost:18099/users/abc
# => 422 {"detail":[{"type":"int_parsing","loc":["path","id"],"msg":"Input should be a valid integer, unable to parse string as an integer","input":"abc"}]}

# Search with query parameters and validation
curl "http://localhost:18099/search?q=al&limit=2"
# => [{"id":1,"name":"alice","age":30},{"id":2,"name":"bob",25}]

# Create a new user via POST
curl -X POST -H "Content-Type: application/json" -d '{"name":"zoe","age":31}' http://localhost:18099/users
# => 201 {"id":4,"name":"zoe","age":31}

# Access the auto-generated OpenAPI documentation
open http://localhost:18099/openapi.json

# Explore with Swagger UI
open http://localhost:18099/docs
```

## Architecture: Two Front Doors, One SQL Brain

**quackapi** is built around a single, central SQL macro: `handle_request(method, path, headers, body)`. This macro is the "brain" that performs all routing, validation, and determines the handler logic.

1.  **Pure-SQL Reference (Tier 1):** The entire framework operates as pure DuckDB SQL (`framework.sql`). You can call `handle_request(...)` directly from any SQL client, making it a powerful API surface even without an external server process. This serves as the executable specification for the C++ extension.

2.  **Compiled C++ Extension (Tier 2):** For browser-native, high-performance serving, the `ext-cpp` directory houses a compiled C++ DuckDB extension. This extension embeds a raw-socket pthread HTTP server within the DuckDB process itself (`serve_brain(port, db_path) + block_forever()`). This C layer efficiently handles network I/O and then delegates to the *same* `handle_request` SQL brain to process the request. It's the uvicorn-equivalent, but with zero framework logic in C—just byte shuffling and SQL dispatch.

## Composability: Beyond the Python `venv`

Since **quackapi** handlers are pure SQL, composing with other DuckDB capabilities is as simple as `INSTALL ...; LOAD ...` an extension. The typical FastAPI production `venv` often ranges from 300MB-1GB; a feature-rich **quackapi** deployment, including numerous extensions, typically weighs in at 155-170MB.

This enables powerful integrations directly within your API:

*   **`json_schema`**: Declarative JSON document validation against schemas stored as data.
*   **`finetype`**: Semantic type inference (e.g., classifying IP addresses, emails) as an endpoint.
*   **`crypto`**: HMAC webhook signing for secure callback endpoints.
*   **`tera`**: Server-rendered HTML templates (Jinja2-like syntax) directly from live table data.
*   **`parser_tools`**: A SQL-linting endpoint using DuckDB's own parser.
*   **`curl_httpfs`**: Parallel HTTP fan-out to multiple upstream URLs within a single request.
*   **`fts`**: BM25 full-text search capabilities with one `PRAGMA`.
*   **`cronjob`**: Scheduled background tasks, effectively a Celery-equivalent inside DuckDB.
*   **`bitfilters`**: Probabilistic membership for rate limiting or deduplication.
*   **`rapidfuzz`**: Typo-tolerant fuzzy lookup for user input.
*   **`markdown`**: Markdown to HTML rendering on the fly.
*   **`postgres`**: Expose live PostgreSQL tables via RESTful endpoints, with **quackapi** validation guarding parameters.

## Verification & Conformance

Every claim in **quackapi** is rigorously verified:

*   **Extensive Testing:** Over 56 `sqllogictest` assertions ensure the SQL brain behaves as expected.
*   **FastAPI Conformance:** An 87-case conformance suite tests against FastAPI's behavior, with 62 exact matches. Documented intentional deviations are explicitly logged (`edges.md`).
*   **Fuzz Testing:** A 100/100 fuzz oracle ensures robustness against malformed inputs.
*   **Pure-SQL Oracle:** The entire framework also exists as pure DuckDB SQL (`framework.sql`), serving as the byte-compared oracle against which the C++ extension is validated.

## Benchmarks: Uvicorn-Class Performance

The compiled C++ extension delivers exceptional performance, achieving throughput squarely in the Uvicorn-class range on Apple Silicon (ab, zero failed requests):

*   **Static Routes (zero DB calls):** ~39-44k req/s
*   **Dynamic Routed & Validated (with handler query):** ~26-35k req/s
*   For comparison, the pure-SQL routing layer, while demonstrating the framework's internal logic, tops out at ~1k req/s. The C++ extension dramatically improves this by moving the routing match loop and validation into native code, while still letting DuckDB execute the handler SQL at ~34k req/s for simple queries.

## Limitations & Edges

Transparency about boundaries is critical. **quackapi** has the following known edges:

*   **Single-Writer Semantics:** DuckDB's single-writer architecture bounds overall write throughput for heavily contentious writes. While internal execution channels and OCC retry mechanisms help, this remains a fundamental constraint.
*   **No WebSockets (Yet):** The transport layer (handshake, framing) is prototyped in C++, but the full application-level wiring for bi-directional communication is an active roadmap item.
*   **No TLS (Yet):** The built-in server binds to localhost by default without TLS. It is designed to be fronted by a reverse proxy (e.g., Caddy, Nginx) or integrated into a private Tailscale network (`quackscale`).
*   **Multipart Upload Limit:** The C reader currently buffers to a fixed ~64 KB. Larger multipart file uploads are not yet fully streamed or supported.
*   **Dependency Injection Teardown:** Dependency injection is supported, but a guaranteed `finally` block for resource teardown (e.g., database session closing) is not strictly enforced in the stateless, one-shot dispatch model.
*   **C Bug Crashes:** As a C++ extension, a bug in the C code can crash the entire DuckDB process (no sandbox). Defensive coding and process supervision are key mitigations; Rust offers a memory-safe alternative for this layer.

For a living ledger of every hypothesis, probe, and verdict, see [`edges.md`](./edges.md).

## Roadmap

**quackapi** is under active development. Key upcoming features include:

*   **Full WebSockets:** Completing the application-level wiring for robust, bi-directional communication.
*   **TLS Integration:** Native TLS support for secure endpoints.
*   **Windows Support:** Expanding platform compatibility.
*   **Policy-Based Authorization:** Custom SQL DDL for defining access control policies.

## License

**quackapi** is released under the [MIT License](./ext-cpp/duckdb/LICENSE).
