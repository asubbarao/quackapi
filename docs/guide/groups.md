# CREATE GROUP — prefix, shared auth, versioning

A **group** is FastAPI’s `APIRouter`: a path prefix, optional default auth, and OpenAPI tags. Member routes join the prefix and inherit auth unless they override it.

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## Create a versioned API surface

```sql
CREATE AUTH api AS API_KEY;
SELECT * FROM quackapi_add_api_key('api', 'k-g', 'u');

CREATE GROUP v1 WITH (prefix='/api/v1', auth=api, tags='items,v1');

CREATE ROUTE items_list GET '/items' GROUP v1 AS
SELECT 1 AS id, 'x' AS name;

CREATE ROUTE items_get GET '/items/:id' IN GROUP v1 AS
SELECT $id::INTEGER AS id;
-- IN GROUP is a synonym for GROUP
```

Served paths:

| Route | Full path | Auth |
|-------|-----------|------|
| `items_list` | `GET /api/v1/items` | `api` |
| `items_get` | `GET /api/v1/items/:id` | `api` |

```sh
curl http://127.0.0.1:8000/api/v1/items
# HTTP 401  (missing key)

curl http://127.0.0.1:8000/api/v1/items -H 'X-API-Key: k-g'
# [{"id":1,"name":"x"}]
# HTTP 200

curl http://127.0.0.1:8000/api/v1/items/42 -H 'X-API-Key: k-g'
# [{"id":42}]

curl http://127.0.0.1:8000/items -H 'X-API-Key: k-g'
# HTTP 404  (bare path is not mounted)
```

---

## Relative paths

Member patterns **without** a leading slash join cleanly:

```sql
CREATE ROUTE items_alt GET 'alt' GROUP v1 AS SELECT 1 AS n;
-- served as GET /api/v1/alt
```

Ungrouped routes still require an absolute path starting with `/`.

---

## Route-level REQUIRE wins

```sql
CREATE AUTH other AS API_KEY;

CREATE ROUTE override GET '/admin' GROUP v1 REQUIRE other AS
SELECT 1 AS n;
-- full path /api/v1/admin, require_auth = other (not api)
```

---

## Inspect

```sql
SELECT name, prefix, require_auth, tags, members
FROM quackapi_groups();

SELECT name, pattern, require_auth, group_name, tags
FROM quackapi_routes()
WHERE group_name = 'v1';
```

---

## Grammar forms

```sql
CREATE [OR REPLACE] [API] GROUP <name>
  WITH (prefix='/abs' [, auth=<name> | require=<name>] [, tags='a,b'] );

-- alternate keyword style:
CREATE API GROUP <name> PREFIX '/p' [TAGS 't'] [REQUIRE <auth>];

DROP [API] GROUP <name>;
```

`CREATE GROUP` and `CREATE API GROUP` are synonyms.  
`auth=` and `require=` in `WITH` are synonyms.

---

## Versioning recipe

```sql
CREATE GROUP v1 WITH (prefix='/api/v1', auth=api, tags='v1');
CREATE GROUP v2 WITH (prefix='/api/v2', auth=api, tags='v2');

CREATE ROUTE items_v1 GET '/items' GROUP v1 AS SELECT …;
CREATE ROUTE items_v2 GET '/items' GROUP v2 AS SELECT …;  -- new shape
```

Clients pin a version by path. Tags flow into OpenAPI.

---

## Next

- [Auth](auth.md)  
- [OpenAPI](openapi.md)
