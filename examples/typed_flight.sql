-- Typed SQL results over Flight SQL, using a separate DuckDB server.
--
-- Verified with DuckDB v1.5.6 (069cc9f9b5), DuckFlight v0.1.12,
-- adbc_scanner (extension build 9723c3c), and the
-- adbc-driver-flightsql Python package 1.12.0 for its native driver library.
-- The SQL client needs the adbc_scanner extension and the platform-specific
-- libadbc_driver_flightsql shared library. Install/configure these in the
-- client environment; this example does not install anything globally.
--
-- Before starting the server, create a DuckFlight auth config with a user
-- allowed to connect. See the DuckFlight authentication documentation:
-- https://github.com/sidequery/duckflight-extension#authentication
-- Keep credentials out of SQL files and source control. For non-loopback
-- connections, configure TLS and use the matching secure Flight URL.

-- SERVER PROCESS: run in the DuckDB process that owns the source database.
LOAD duckflight;
LOAD sitting_duck;

SELECT duckflight_core_status();
SELECT *
FROM duckflight_flight_serve(
  '127.0.0.1:0',
  '/path/to/duckflight-auth.toml'
);

-- CLIENT PROCESS: use the returned listener port. The driver path below is a
-- placeholder; use libadbc_driver_flightsql.dylib on macOS or .so on Linux.
LOAD adbc_scanner;

ATTACH 'grpc://127.0.0.1:<returned-port>' AS flight (
  TYPE adbc,
  DRIVER '/path/to/libadbc_driver_flightsql.dylib',
  USERNAME 'flight_user',
  PASSWORD '<secret-from-your-local-config>'
);

-- Declare the result schema for each arbitrary SQL statement. These values
-- retain their DuckDB types through the Flight SQL/ADBC roundtrip.
SELECT d, typeof(d), ts, typeof(ts), nested, typeof(nested),
       hex(blob), typeof(blob)
FROM adbc_scan(
  'flight',
  $query$
    SELECT 10.125::DECIMAL(9,3) AS d,
           TIMESTAMPTZ '2026-10-14 01:02:03.456+00' AS ts,
           {'x': 1::INTEGER, 'ys': [2::INTEGER, 3::INTEGER]} AS nested,
           from_hex('00FF') AS blob
  $query$,
  columns := {
    'd': 'DECIMAL(9,3)',
    'ts': 'TIMESTAMP WITH TIME ZONE',
    'nested': 'STRUCT(x INTEGER, ys INTEGER[])',
    'blob': 'BLOB'
  }
);

-- The same mechanism can run a query against source files on the server.
-- read_ast's SEMANTIC_TYPE enum is explicitly cast to VARCHAR here because
-- the ADBC result schema declares VARCHAR; this keeps the label but loses the
-- DuckDB enum type identity. Replace the path with a source file on the server.
SELECT node_id, typeof(node_id), semantic_type_name,
       typeof(semantic_type_name), parameters, typeof(parameters),
       start_line, typeof(start_line)
FROM adbc_scan(
  'flight',
  $query$
    SELECT node_id,
           CAST(semantic_type AS VARCHAR) AS semantic_type_name,
           parameters,
           start_line
    FROM read_ast('/path/to/source.cpp', 'cpp')
    WHERE len(parameters) > 0
    LIMIT 2
  $query$,
  columns := {
    'node_id': 'BIGINT',
    'semantic_type_name': 'VARCHAR',
    'parameters': 'STRUCT(name VARCHAR, type VARCHAR)[]',
    'start_line': 'UINTEGER'
  }
);

-- One ADBC attachment supports one active operation at a time. This example
-- is a direct query roundtrip, not a dynamic per-row fan-out interface.
DETACH flight;
