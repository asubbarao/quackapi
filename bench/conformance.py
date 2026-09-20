#!/usr/bin/env python3
from __future__ import annotations

import argparse
import http.client
import json
import sys


CASES = [
    ("root_ok", "GET", "/?token=jessica", {}, 200, {"message": "Hello Bigger Applications!"}),
    ("missing_query", "GET", "/", {}, 422, None),
    ("wrong_query", "GET", "/?token=wrong", {}, 400, {"detail": "No Jessica token provided"}),
    ("user_ok", "GET", "/users/rick?token=jessica", {}, 200, {"username": "rick"}),
    ("item_ok", "GET", "/items/plumbus?token=jessica", {"X-Token": "fake-super-secret-token"}, 200, {"name": "Plumbus", "item_id": "plumbus"}),
    ("missing_header", "GET", "/items/plumbus?token=jessica", {}, 422, None),
    ("item_missing", "GET", "/items/nope?token=jessica", {"X-Token": "fake-super-secret-token"}, 404, {"detail": "Item not found"}),
    ("update_forbidden", "PUT", "/items/gun?token=jessica", {"X-Token": "fake-super-secret-token"}, 403, {"detail": "You can only update the item: plumbus"}),
    ("admin_ok", "POST", "/admin/?token=jessica", {"X-Token": "fake-super-secret-token"}, 200, {"message": "Admin getting schwifty"}),
]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--stack", required=True)
    parser.add_argument("--port", type=int, required=True)
    args = parser.parse_args()
    results = []
    for name, method, path, headers, expected_status, expected_body in CASES:
        connection = http.client.HTTPConnection("127.0.0.1", args.port, timeout=5)
        try:
            connection.request(method, path, headers=headers)
            response = connection.getresponse()
            raw = response.read()
            try:
                body = json.loads(raw)
            except json.JSONDecodeError:
                body = raw.decode("utf-8", "replace")
            body_ok = expected_body is None and isinstance(body, dict) and "detail" in body
            if expected_body is not None:
                body_ok = body == expected_body
            results.append({"name": name, "expected_status": expected_status, "actual_status": response.status, "expected_body": expected_body, "actual_body": body, "passed": response.status == expected_status and body_ok})
        except Exception as exc:
            results.append({"name": name, "passed": False, "error": repr(exc)})
        finally:
            connection.close()
    passed = sum(case["passed"] for case in results)
    print(json.dumps({"stack": args.stack, "passed": passed, "total": len(results), "cases": results}, separators=(",", ":")))
    failed = [case["name"] for case in results if not case["passed"]]
    if failed:
        print(f"FAIL: {args.stack} violated the response contract: {', '.join(failed)}", file=sys.stderr)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
