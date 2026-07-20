# Auth — API keys, JWT, and REQUIRE

Protect a route with three steps:

1. **`CREATE AUTH`** — register a scheme  
2. **Issue credentials** — API keys via `quackapi_add_api_key`, or mint a JWT with your secret  
3. **`REQUIRE <scheme>`** on the route (or inherit from a [GROUP](groups.md))

All examples run against `build/release/duckdb -unsigned` with `LOAD quackapi;`.

---

## API key

```sql
CREATE AUTH site AS API_KEY;
-- default header: X-API-Key

SELECT * FROM quackapi_add_api_key('site', 'k-secret', 'alice');
-- stores SHA-256 of the key only; returns subject 'alice'

CREATE ROUTE secure GET '/secure' REQUIRE site AS
SELECT true AS ok, 'alice' AS sub;
```

```sh
curl http://127.0.0.1:8000/secure -H 'X-API-Key: k-secret'
# [{"ok":true,"sub":"alice"}]
# HTTP 200

curl http://127.0.0.1:8000/secure
# {"detail":"Not authenticated"}
# HTTP 401
# WWW-Authenticate: ApiKey

curl http://127.0.0.1:8000/secure -H 'X-API-Key: wrong'
# HTTP 401
```

### Custom header name

```sql
CREATE AUTH custom AS API_KEY ( HEADER 'X-Custom-Key' );
```

Inspect schemes (secrets never appear):

```sql
SELECT name, kind, header FROM quackapi_auths();
-- site | API_KEY | X-API-Key
```

---

## JWT (HS256)

```sql
CREATE AUTH jwt_auth AS JWT ( SECRET 'conformance-secret' );
-- optional: ALGORITHM HS256  (only HS256 is accepted)

CREATE ROUTE jwt_route GET '/jwt' REQUIRE jwt_auth AS
SELECT 'ok' AS status;
```

Clients send `Authorization: Bearer <token>`.

```sh
# Missing / bad token → 401
curl http://127.0.0.1:8000/jwt
# HTTP 401

# Valid HS256 JWT with secret conformance-secret → 200
curl http://127.0.0.1:8000/jwt -H "Authorization: Bearer $JWT"
# [{"status":"ok"}]
```

Mint a test token (Python):

```python
import base64, hashlib, hmac, json, time

def b64url(data: bytes) -> str:
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode()

secret = b"conformance-secret"
header = b64url(json.dumps({"alg": "HS256", "typ": "JWT"}, separators=(",", ":")).encode())
payload = b64url(json.dumps({"sub": "alice", "exp": int(time.time()) + 3600}, separators=(",", ":")).encode())
sig = b64url(hmac.new(secret, f"{header}.{payload}".encode(), hashlib.sha256).digest())
print(f"{header}.{payload}.{sig}")
```

Only **`ALGORITHM HS256`** is supported. RS256 / OIDC discovery are not built yet.

---

## Claims in handlers

Verified JWT (and API-key) claims bind as **`$claims_<name>`**.

| Claim key in token | SQL parameter |
|--------------------|---------------|
| `sub` | `$claims_sub` |
| `tenant` | `$claims_tenant` |
| `role` | `$claims_role` |
| any other key | `$claims_<key>` |

Missing claim → SQL `NULL` (not 422). Example:

```sql
CREATE ROUTE me GET '/me' REQUIRE site AS
SELECT $claims_sub AS user;
```

Claims power [row access & masking policies](policies.md).

---

## REQUIRE placement

```sql
CREATE ROUTE me2 GET '/me2' STATUS 201 REQUIRE site AS
SELECT $claims_sub AS user;
```

`STATUS` and `REQUIRE` may appear in either order before `AS`.

A non-existent scheme name is allowed at `CREATE` time (fail closed at request with auth error). Prefer creating the auth first.

Group-level default auth: [CREATE GROUP](groups.md). Route-level `REQUIRE` wins over the group.

---

## Replace and drop

```sql
CREATE OR REPLACE AUTH site AS API_KEY ( HEADER 'X-Site-Key' );
DROP AUTH site;
```

```sql
SELECT * FROM quackapi_add_api_key('site', 'raw-key', 'alice');
-- error if scheme missing or not API_KEY
```

---

## Verify from SQL (optional)

```sql
SELECT (quackapi_verify_auth('site', 'k-secret')).ok;      -- true
SELECT (quackapi_verify_auth('site', 'k-secret')).status;  -- 200
SELECT (quackapi_verify_auth('site', 'wrong')).ok;         -- false
```

---

## Next

- [CREATE GROUP](groups.md) — shared auth on a prefix  
- [Policies](policies.md) — claims-keyed row filters and masks
