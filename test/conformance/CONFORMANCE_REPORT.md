# Conformance Report: quackapi vs FastAPI

Generated: 2026-07-05T14:31:01.040070

Comparison methodology: same HTTP requests replayed against both stacks.
quackapi runs on the C++ extension server (serve_brain_ex).
FastAPI runs via uvicorn with a hand-written mirror app.

## Summary

| Metric | Count |
|--------|-------|
| Total cases | 87 |
| MATCH | 54 |
| DIVERGE | 33 |
| &nbsp;&nbsp;↳ BUG | 13 |
| &nbsp;&nbsp;↳ INTENTIONAL | 13 |
| &nbsp;&nbsp;↳ COSMETIC | 5 |
| &nbsp;&nbsp;↳ FASTAPI-QUIRK | 2 |

## Classification Key

- **BUG**: quackapi behavior is wrong relative to FastAPI and should be fixed
- **INTENTIONAL**: documented design difference between quackapi and FastAPI
- **COSMETIC**: wording / header order difference that does not affect semantics
- **FASTAPI-QUIRK**: FastAPI behavior that is arguably wrong/surprising (e.g. trailing-slash 307 redirect)

## BUG-class Divergences (13 total)

These are cases where quackapi returns a different result and should be fixed.

### `get_user_bad_negative` — GET /users/-1
- **Notes**: STATUS: qk=404 fa=200
- **quackapi**: status=404 ct=application/json
  ```
  {"detail":"Not Found"}
  ```
- **FastAPI**: status=200 ct=application/json
  ```
  null
  ```

### `get_user_overflow` — GET /users/99999999999999999999
- **Notes**: STATUS: qk=404 fa=200
- **quackapi**: status=404 ct=application/json
  ```
  {"detail":"Not Found"}
  ```
- **FastAPI**: status=200 ct=application/json
  ```
  null
  ```

### `get_user_zero` — GET /users/0
- **Notes**: STATUS: qk=404 fa=200
- **quackapi**: status=404 ct=application/json
  ```
  {"detail":"Not Found"}
  ```
- **FastAPI**: status=200 ct=application/json
  ```
  null
  ```

### `search_empty_q` — GET /search?q=
- **Notes**: STATUS: qk=422 fa=200
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"missing","loc":["query","q"],"msg":"Field required","input":null}]}
  ```
- **FastAPI**: status=200 ct=application/json
  ```
  [{"id":1,"name":"alice","age":30},{"id":2,"name":"bob","age":25},{"id":3,"name":"carol","age":40}]
  ```

### `post_users_null_body` — POST /users
- **Notes**: 422 locs differ: qk=['["body", "age"]', '["body", "name"]'] fa=['["body"]']
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"missing","loc":["body","name"],"msg":"Field required","input":null},{"type":"missing","loc":["body","age"],"msg":"Field required","input":null}]}
  ```
- **FastAPI**: status=422 ct=application/json
  ```
  {"detail":[{"type":"missing","loc":["body"],"msg":"Field required","input":null}]}
  ```

### `post_users_empty_body` — POST /users
- **Notes**: 422 locs differ: qk=['["body", "age"]', '["body", "name"]'] fa=['["body"]']
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"missing","loc":["body","name"],"msg":"Field required","input":null},{"type":"missing","loc":["body","age"],"msg":"Field required","input":null}]}
  ```
- **FastAPI**: status=422 ct=application/json
  ```
  {"detail":[{"type":"missing","loc":["body"],"msg":"Field required","input":null}]}
  ```

### `post_users_wrong_ct` — POST /users
- **Notes**: STATUS: qk=201 fa=422
- **quackapi**: status=201 ct=application/json
  ```
  {"id":101,"name":"x","age":5}
  ```
- **FastAPI**: status=422 ct=application/json
  ```
  {"detail":[{"type":"model_attributes_type","loc":["body"],"msg":"Input should be a valid dictionary or object to extract fields from","input":"{\"name\":\"x\",\"age\":5}"}]}
  ```

### `post_users_array_body` — POST /users
- **Notes**: STATUS: qk=201 fa=422
- **quackapi**: status=201 ct=application/json
  ```
  {"id":102,"name":"x","age":5}
  ```
- **FastAPI**: status=422 ct=application/json
  ```
  {"detail":[{"type":"model_attributes_type","loc":["body"],"msg":"Input should be a valid dictionary or object to extract fields from","input":[{"name":"x","age":5}]}]}
  ```

### `post_users_malformed_json` — POST /users
- **Notes**: 422 locs differ: qk=['["body", "age"]', '["body", "name"]'] fa=['["body", 1]']
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"missing","loc":["body","name"],"msg":"Field required","input":null},{"type":"missing","loc":["body","age"],"msg":"Field required","input":null}]}
  ```
- **FastAPI**: status=422 ct=application/json
  ```
  {"detail":[{"type":"json_invalid","loc":["body",1],"msg":"JSON decode error","input":{},"ctx":{"error":"Expecting property name enclosed in double quotes"}}]}
  ```

### `upload_malformed_mp` — POST /upload
- **Notes**: STATUS: qk=422 fa=400
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"multipart_parse","loc":["body"],"msg":"Malformed multipart body: boundary not found"}]}
  ```
- **FastAPI**: status=400 ct=application/json
  ```
  {"detail":"There was an error parsing the body"}
  ```

### `post_users_age_bool_true` — POST /users
- **Notes**: STATUS: qk=422 fa=201
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"int_parsing","loc":["body","age"],"msg":"Input should be a valid integer, unable to parse string as an integer","input":"true"}]}
  ```
- **FastAPI**: status=201 ct=application/json
  ```
  {"id":102,"name":"x","age":1}
  ```

### `post_users_age_bool_false` — POST /users
- **Notes**: STATUS: qk=422 fa=201
- **quackapi**: status=422 ct=application/json
  ```
  {"detail":[{"type":"int_parsing","loc":["body","age"],"msg":"Input should be a valid integer, unable to parse string as an integer","input":"false"}]}
  ```
- **FastAPI**: status=201 ct=application/json
  ```
  {"id":103,"name":"x","age":0}
  ```

### `post_users_age_overflow` — POST /users
- **Notes**: STATUS: qk=500 fa=201
- **quackapi**: status=500 ct=application/json
  ```
  {"error":"handler"}
  ```
- **FastAPI**: status=201 ct=application/json
  ```
  {"id":106,"name":"x","age":99999999999999999999}
  ```


## INTENTIONAL Divergences (13 total)

### `health_head` — HEAD /health
- **Notes**: STATUS: qk=200 fa=405
- qk: 200 | fa: 405

### `health_post_405` — POST /health
- **Notes**: Allow header: qk=['GET', 'HEAD'] fa=['GET']
- qk: 405 | fa: 405

### `health_delete_405` — DELETE /health
- **Notes**: Allow header: qk=['GET', 'HEAD'] fa=['GET']
- qk: 405 | fa: 405

### `health_options_405` — OPTIONS /health
- **Notes**: Allow header: qk=['GET', 'HEAD'] fa=['GET']
- qk: 405 | fa: 405

### `health_put_405` — PUT /health
- **Notes**: Allow header: qk=['GET', 'HEAD'] fa=['GET']
- qk: 405 | fa: 405

### `get_user_404` — GET /users/9999
- **Notes**: STATUS: qk=404 fa=200
- qk: 404 | fa: 200

### `get_user_head` — HEAD /users/1
- **Notes**: STATUS: qk=200 fa=405
- qk: 200 | fa: 405

### `list_users_head` — HEAD /users
- **Notes**: STATUS: qk=200 fa=405
- qk: 200 | fa: 405

### `method_mismatch_users_delete` — DELETE /users
- **Notes**: Allow header: qk=['GET', 'HEAD', 'POST'] fa=['GET']
- qk: 405 | fa: 405

### `method_mismatch_users_put` — PUT /users
- **Notes**: Allow header: qk=['GET', 'HEAD', 'POST'] fa=['GET']
- qk: 405 | fa: 405

### `method_mismatch_health_delete` — DELETE /health
- **Notes**: Allow header: qk=['GET', 'HEAD'] fa=['GET']
- qk: 405 | fa: 405

### `method_mismatch_getuser_post` — POST /users/1
- **Notes**: Allow header: qk=['GET', 'HEAD'] fa=['GET']
- qk: 405 | fa: 405

### `openapi_json` — GET /openapi.json
- **Notes**: openapi version: qk=3.0.0 fa=3.1.0
- qk: 200 | fa: 200


## COSMETIC Divergences (5 total)

| Case ID | Method | Path | Notes |
|---------|--------|------|-------|
| `search_repeated_param` | GET | `/search?q=a&q=b` | BODY: qk=[{"id": 1, "name": "alice", "age": 30}] fa=[{"id": 2, "name": "bob", "age": 25}] |
| `post_users_ct_charset` | POST | `/users` | BODY: qk={"id": 103, "name": "x", "age": 5} fa={"id": 101, "name": "x", "age": 5} |
| `post_users_age_neg` | POST | `/users` | BODY: qk={"id": 106, "name": "x", "age": -5} fa={"id": 107, "name": "x", "age": -5} |
| `post_users_null_name` | POST | `/users` | 422 types differ: qk=['missing'] fa=['string_type'] |
| `post_users_age_string_int` | POST | `/users` | BODY: qk={"id": 107, "name": "x", "age": 5} fa={"id": 108, "name": "x", "age": 5} |

## FASTAPI-QUIRK Divergences (2 total)

### `list_users_trailing_slash` — GET /users/
- **Notes**: STATUS: qk=200 fa=307
- qk: 200 | fa: 307
- fa body: ``

### `health_trailing_slash` — GET /health/
- **Notes**: STATUS: qk=200 fa=307
- qk: 200 | fa: 307
- fa body: ``


## Matching Cases (54 total)

| Case ID | Method | Path |
|---------|--------|------|
| `health_get` | GET | `/health` |
| `get_user_1` | GET | `/users/1` |
| `get_user_2` | GET | `/users/2` |
| `get_user_3` | GET | `/users/3` |
| `get_user_bad_int` | GET | `/users/abc` |
| `get_user_bad_float` | GET | `/users/1.5` |
| `get_user_leading_zero` | GET | `/users/01` |
| `get_user_space` | GET | `/users/%20` |
| `list_users` | GET | `/users` |
| `search_happy` | GET | `/search?q=al&limit=2` |
| `search_no_limit` | GET | `/search?q=b` |
| `search_limit_max` | GET | `/search?q=a&limit=100` |
| `search_limit_over` | GET | `/search?q=a&limit=101` |
| `search_limit_999` | GET | `/search?q=hi&limit=999` |
| `search_missing_q` | GET | `/search?limit=5` |
| `search_limit_bad_int` | GET | `/search?q=a&limit=abc` |
| `search_limit_float` | GET | `/search?q=a&limit=1.5` |
| `search_limit_zero` | GET | `/search?q=a&limit=0` |
| `search_limit_neg` | GET | `/search?q=a&limit=-1` |
| `search_no_params` | GET | `/search` |
| `post_users_happy` | POST | `/users` |
| `post_users_missing_age` | POST | `/users` |
| `post_users_missing_name` | POST | `/users` |
| `post_users_both_missing` | POST | `/users` |
| `post_users_bad_age_type` | POST | `/users` |
| `post_users_age_float_str` | POST | `/users` |
| `not_found_root` | GET | `/` |
| `not_found_path` | GET | `/nope` |
| `not_found_deep` | GET | `/users/1/deep/nesting/not/registered` |
| `not_found_head` | HEAD | `/nonexistent-xyz` |
| `secure_happy` | GET | `/secure` |
| `secure_missing_key` | GET | `/secure` |
| `profile_happy` | GET | `/profile` |
| `profile_missing_cookie` | GET | `/profile` |
| `form_submit_happy` | POST | `/form-submit` |
| `form_submit_bad_age` | POST | `/form-submit` |
| `form_submit_missing_age` | POST | `/form-submit` |
| `form_submit_url_encoded` | POST | `/form-submit` |
| `redirect_old_home` | GET | `/old-home` |
| `login_set_cookie` | POST | `/login` |
| `upload_happy` | POST | `/upload` |
| `upload_missing_file` | POST | `/upload` |
| `upload_no_ct` | POST | `/upload` |
| `docs_get` | GET | `/docs` |
| `search_limit_1e2` | GET | `/search?q=a&limit=1e2` |
| `search_limit_padded` | GET | `/search?q=a&limit=+5` |
| `search_limit_str_int` | GET | `/search?q=a&limit=5` |
| `post_users_extra_fields` | POST | `/users` |
| `get_user_unicode_path` | GET | `/users/%E4%B8%AD%E6%96%87` |
| `search_q_unicode` | GET | `/search?q=%E4%B8%AD` |
| `post_users_name_long` | POST | `/users` |
| `events_stream` | GET | `/events` |
| `not_found_method_delete_nonexistent` | DELETE | `/nonexistent-xyz-123` |
| `get_user_plus_in_path` | GET | `/users/1+2` |

## Re-run Command

```bash
cd /Users/aloksubbarao/quackapi
bash test/conformance/run_conformance.sh
```

Or to run driver only against existing servers:
```bash
cd test/conformance
python3 driver.py --qk http://127.0.0.1:18500 --fa http://127.0.0.1:18501
python3 generate_report.py
```
