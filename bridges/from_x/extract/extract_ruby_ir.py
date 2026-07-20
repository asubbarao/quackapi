#!/usr/bin/env python3
"""Extract unified IR for Ruby web frameworks from sitting_duck ASTs.

Parser: sitting_duck read_ast(..., language:='ruby') — verified supported.
Expands Rails `resources` / `resource` to RESTful routes.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path

CORPUS = Path("/tmp/quackapi_corpus")
RUBY = CORPUS / "ruby"
OUT_ROUTES = CORPUS / "ir_ruby_routes.parquet"
OUT_MODELS = CORPUS / "ir_ruby_models.parquet"
DUCKDB = os.environ.get("DUCKDB", "duckdb")

# Rails 7 default resources actions → (method, path_suffix, action)
# path_suffix uses {name} for collection path base and {param} for member id.
RESOURCES_7 = [
    ("GET",    "",           "index"),
    ("POST",   "",           "create"),
    ("GET",    "/new",       "new"),
    ("GET",    "/:id",       "show"),
    ("GET",    "/:id/edit",  "edit"),
    ("PATCH",  "/:id",       "update"),
    ("PUT",    "/:id",       "update"),
    ("DELETE", "/:id",       "destroy"),
]
# User asked for "7 RESTful routes" — classic 7 is index/create/new/show/edit/update/destroy
# (PUT+PATCH both map to update; we emit both HTTP verbs for update fidelity but document
# the canonical 7 actions). Actions set for only/except filtering:
RESOURCE_ACTIONS = {
    "index":   [("GET",    "")],
    "create":  [("POST",   "")],
    "new":     [("GET",    "/new")],
    "show":    [("GET",    "/:id")],
    "edit":    [("GET",    "/:id/edit")],
    "update":  [("PATCH",  "/:id"), ("PUT", "/:id")],
    "destroy": [("DELETE", "/:id")],
}
SINGULAR_RESOURCE_ACTIONS = {
    "show":    [("GET",    "")],
    "create":  [("POST",   "")],
    "new":     [("GET",    "/new")],
    "edit":    [("GET",    "/edit")],
    "update":  [("PATCH",  ""), ("PUT", "")],
    "destroy": [("DELETE", "")],
}

DEFAULT_7 = ["index", "create", "new", "show", "edit", "update", "destroy"]


def run_sql(sql: str) -> str:
    p = subprocess.run(
        [DUCKDB, "-csv", "-c", sql],
        capture_output=True,
        text=True,
    )
    if p.returncode != 0:
        raise RuntimeError(f"duckdb failed:\n{p.stderr}\nSQL:\n{sql[:500]}")
    return p.stdout


def sitting_duck_calls(glob_or_path: str, names: list[str] | None = None) -> list[dict]:
    """Return call nodes from sitting_duck AST."""
    name_filter = ""
    if names:
        inlist = ", ".join("'" + n + "'" for n in names)
        name_filter = f"AND name IN ({inlist})"
    sql = f"""
LOAD sitting_duck;
COPY (
  SELECT
    file_path,
    name AS call_name,
    start_line,
    end_line,
    peek,
    parent_id,
    node_id,
    depth
  FROM read_ast('{glob_or_path}', 'ruby', peek := 'full')
  WHERE type = 'call' {name_filter}
  ORDER BY file_path, start_line, node_id
) TO '/tmp/quackapi_corpus/scratch/ruby_calls.csv' (HEADER, DELIMITER ',');
"""
    run_sql(sql)
    import csv
    rows = []
    with open("/tmp/quackapi_corpus/scratch/ruby_calls.csv") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def sitting_duck_classes(glob_or_path: str) -> list[dict]:
    sql = f"""
LOAD sitting_duck;
COPY (
  SELECT file_path, name AS class_name, start_line, end_line, node_id, peek
  FROM read_ast('{glob_or_path}', 'ruby', peek := 'full')
  WHERE type = 'class' AND name IS NOT NULL AND name <> ''
  ORDER BY file_path, start_line
) TO '/tmp/quackapi_corpus/scratch/ruby_classes.csv' (HEADER, DELIMITER ',');
"""
    run_sql(sql)
    import csv
    with open("/tmp/quackapi_corpus/scratch/ruby_classes.csv") as f:
        return list(csv.DictReader(f))


def strip_symbol(s: str) -> str:
    s = s.strip()
    if s.startswith(":"):
        s = s[1:]
    return s.strip("'\"")


def header_line(peek: str) -> str:
    """First logical line of a call — excludes nested do-block children.

    sitting_duck peek for multi-line `resources :x do ... end` includes the
    whole block; nested `only:` / `except:` would otherwise be mis-attributed.
    """
    # Cut at ` do` that opens a block, else first newline
    m = re.match(r"^(.*?)(?:\s+do\b|\n|$)", peek, re.DOTALL)
    if m:
        return m.group(1).strip()
    return peek.split("\n", 1)[0].strip()


def parse_only_except(peek: str) -> tuple[set[str] | None, set[str] | None]:
    """Parse only: [...] / except: [...] from a resources call header only."""
    head = header_line(peek)
    only = None
    except_ = None
    m = re.search(r"only:\s*\[([^\]]*)\]", head)
    if m:
        only = set(re.findall(r":(\w+)", m.group(1)))
    m = re.search(r"except:\s*\[([^\]]*)\]", head)
    if m:
        except_ = set(re.findall(r":(\w+)", m.group(1)))
    return only, except_


def parse_param(peek: str) -> str:
    m = re.search(r"param:\s*:(\w+)", header_line(peek))
    return m.group(1) if m else "id"


def parse_resource_name(peek: str, call: str) -> str | None:
    # resources :articles, ...  OR resource :user — header only
    head = header_line(peek)
    m = re.search(rf"{call}\s+:(\w+)", head)
    if m:
        return m.group(1)
    m = re.search(rf"{call}\s+['\"](\w+)['\"]", head)
    if m:
        return m.group(1)
    return None


def parse_to_handler(peek: str) -> str | None:
    # get '/health', to: 'health#show'
    m = re.search(r"to:\s*['\"]([^'\"]+)['\"]", peek)
    if m:
        return m.group(1)
    # get 'products/:id' => 'catalog#view'
    m = re.search(r"=>\s*['\"]([^'\"]+)['\"]", peek)
    if m:
        return m.group(1)
    return None


def parse_path_arg(peek: str, method: str) -> str | None:
    # get '/health', ...  or get :feed
    m = re.search(rf"{method}\s+['\"]([^'\"]+)['\"]", peek)
    if m:
        return m.group(1)
    m = re.search(rf"{method}\s+:(\w+)", peek)
    if m:
        return m.group(1)  # symbol path → action name; caller decides
    return None


def actions_for(only: set[str] | None, except_: set[str] | None) -> list[str]:
    acts = list(DEFAULT_7)
    if only is not None:
        acts = [a for a in acts if a in only]
    if except_ is not None:
        acts = [a for a in acts if a not in except_]
    return acts


def expand_resources(
    name: str,
    *,
    singular: bool = False,
    only: set[str] | None = None,
    except_: set[str] | None = None,
    param: str = "id",
    prefix: str = "",
) -> list[tuple[str, str, str]]:
    """Return list of (method, path, action)."""
    acts = actions_for(only, except_)
    table = SINGULAR_RESOURCE_ACTIONS if singular else RESOURCE_ACTIONS
    out = []
    base = f"{prefix}/{name}" if not prefix.endswith(f"/{name}") else prefix
    # prefix already includes leading path; name is resource segment
    base = (prefix.rstrip("/") + "/" + name).replace("//", "/")
    if not base.startswith("/"):
        base = "/" + base
    for action in acts:
        if action not in table:
            continue
        for method, suffix in table[action]:
            path = base + suffix.replace(":id", f":{param}")
            # singular resource has no collection pluralization path difference beyond name
            out.append((method, path, action))
    return out


def controller_for(name: str, singular: bool = False) -> str:
    # articles → articles, user (singular resource) → users is wrong; singular resource :user → users#?
    # Rails: resource :user → UsersController (pluralized controller name still)
    # Simplified: pluralize by adding s if missing for singular resource common case
    if singular:
        # user → users, follow → follows
        if not name.endswith("s"):
            return name + "s"
    return name


def extract_rails_routes(file_path: str, framework: str = "rails") -> list[dict]:
    calls = sitting_duck_calls(
        file_path,
        ["resources", "resource", "get", "post", "put", "patch", "delete", "scope", "namespace", "devise_for"],
    )
    routes: list[dict] = []
    # Track scope prefixes by nesting via end_line stack
    # Simple approach: parse scope prefixes from peeks; maintain stack of (end_line, prefix)
    scope_stack: list[tuple[int, str]] = []  # (end_line, prefix_segment)

    def current_prefix() -> str:
        parts = [p for _, p in scope_stack if p]
        if not parts:
            return ""
        return "/" + "/".join(parts)

    for c in calls:
        peek = c["peek"]
        line = int(c["start_line"])
        end = int(c["end_line"])
        name = c["call_name"]
        # Pop scopes that ended before this line
        scope_stack = [(e, p) for (e, p) in scope_stack if e >= line]

        if name == "scope":
            # scope :api  or scope '/api'
            m = re.search(r"scope\s+:(\w+)", peek)
            seg = m.group(1) if m else None
            if not seg:
                m = re.search(r"scope\s+['\"]([^'\"]+)['\"]", peek)
                seg = m.group(1).strip("/") if m else None
            if seg and " do" in peek:
                scope_stack.append((end, seg))
            continue

        if name == "namespace":
            m = re.search(r"namespace\s+:(\w+)", peek)
            if m and " do" in peek:
                scope_stack.append((end, m.group(1)))
            continue

        prefix = current_prefix()

        if name in ("resources", "resource"):
            singular = name == "resource"
            res = parse_resource_name(peek, name)
            if not res:
                continue
            only, except_ = parse_only_except(peek)
            param = parse_param(peek)
            ctrl = controller_for(res, singular=singular)
            expanded = expand_resources(
                res, singular=singular, only=only, except_=except_, param=param, prefix=prefix
            )
            for method, path, action in expanded:
                routes.append(
                    {
                        "framework": framework,
                        "method": method,
                        "path": path,
                        "handler": f"{ctrl}#{action}",
                        "file": file_path,
                        "line": line,
                        "kind": "resources_expanded" if not singular else "resource_expanded",
                        "source_dsl": header_line(peek)[:200],
                        "source_repo": repo_of(file_path),
                        "resource_name": res,
                        "singular": singular,
                        "param": param,
                    }
                )
            # Nested resource blocks: child resources appear as separate call nodes
            # with higher start_line; we don't auto-nest path for children here —
            # nested `resources :comments` inside articles is a separate call without
            # parent path. Fix: if depth/parent implies nesting, prefix with parent.
            # Heuristic: if call is multi-line block and subsequent resources have
            # start_line within end_line, sitting_duck already returns them as separate
            # top-level calls — we need parent path prefix.
            continue

        if name in ("get", "post", "put", "patch", "delete"):
            method = name.upper()
            path_arg = parse_path_arg(peek, name)
            handler = parse_to_handler(peek)
            on_collection = "on: :collection" in peek or "on: :member" in peek
            if path_arg is None:
                continue
            # Symbol-only (get :feed) — member/collection route; needs parent resources path.
            # Leave as action under nearest resources if we can detect; else record as-is.
            if not path_arg.startswith("/") and ":" not in path_arg and "/" not in path_arg:
                # action symbol
                action = path_arg
                # Find enclosing resources by end_line stack of resources blocks
                parent = find_enclosing_resources(calls, line)
                if parent:
                    res_name = parent["name"]
                    pparam = parent["param"]
                    if "on: :collection" in peek:
                        path = f"{prefix}/{res_name}/{action}".replace("//", "/")
                    else:
                        path = f"{prefix}/{res_name}/:{pparam}/{action}".replace("//", "/")
                    if not path.startswith("/"):
                        path = "/" + path
                    handler = handler or f"{controller_for(res_name)}#{action}"
                else:
                    path = f"{prefix}/{action}".replace("//", "/")
                    if not path.startswith("/"):
                        path = "/" + path
                    handler = handler or f"?#{action}"
            else:
                path = path_arg if path_arg.startswith("/") else "/" + path_arg
                path = (prefix + path).replace("//", "/")
                if not handler:
                    # infer from path
                    handler = "inline#call"
            routes.append(
                {
                    "framework": framework,
                    "method": method,
                    "path": path,
                    "handler": handler or "unknown#unknown",
                    "file": file_path,
                    "line": line,
                    "kind": "explicit",
                    "source_dsl": peek.split("\n")[0][:200],
                    "source_repo": repo_of(file_path),
                }
            )
            continue

        if name == "devise_for":
            res = parse_resource_name(peek, "devise_for") or "users"
            # Minimal devise session routes commonly used
            for method, path, action in [
                ("POST", f"{prefix}/{res}/login", "sessions#create"),
                ("DELETE", f"{prefix}/{res}/logout", "sessions#destroy"),
            ]:
                path = path.replace("//", "/")
                routes.append(
                    {
                        "framework": framework,
                        "method": method,
                        "path": path,
                        "handler": action,
                        "file": file_path,
                        "line": line,
                        "kind": "devise_for_approx",
                        "source_dsl": peek.split("\n")[0][:200],
                        "source_repo": repo_of(file_path),
                    }
                )

    # Second pass: nest child resources that appear inside parent resources do...end
    routes = nest_child_resources(calls, routes, framework)
    return routes


def find_enclosing_resources(calls: list[dict], line: int) -> dict | None:
    enclosing = None
    for c in calls:
        if c["call_name"] not in ("resources", "resource"):
            continue
        s, e = int(c["start_line"]), int(c["end_line"])
        if s < line <= e:
            res = parse_resource_name(c["peek"], c["call_name"])
            if res:
                enclosing = {
                    "name": res,
                    "param": parse_param(c["peek"]),
                    "start": s,
                    "end": e,
                    "singular": c["call_name"] == "resource",
                }
    return enclosing


def nest_child_resources(calls: list[dict], routes: list[dict], framework: str) -> list[dict]:
    """Prefix nested resource paths with parent path segments.

    Correct form: /api/articles/:slug/comments (not /articles/:slug/api/comments).
    """
    parents = []
    for c in calls:
        if c["call_name"] not in ("resources", "resource"):
            continue
        s, e = int(c["start_line"]), int(c["end_line"])
        if e <= s:
            continue
        res = parse_resource_name(c["peek"], c["call_name"])
        if not res:
            continue
        parents.append(
            {
                "name": res,
                "param": parse_param(c["peek"]),
                "start": s,
                "end": e,
                "singular": c["call_name"] == "resource",
                "file": c["file_path"],
            }
        )

    def parent_member_base(parent: dict, all_routes: list[dict], child_file: str) -> str | None:
        """e.g. /api/articles/:slug from already-expanded parent routes."""
        parent_routes = [
            x
            for x in all_routes
            if x["file"] == child_file
            and x["line"] == parent["start"]
            and x["kind"] in ("resources_expanded", "resource_expanded")
        ]
        for x in parent_routes:
            if x["handler"].endswith("#show"):
                return x["path"]
        for x in parent_routes:
            if x["handler"].endswith("#index") or x["handler"].endswith("#create"):
                if parent["singular"]:
                    return x["path"]
                return x["path"].rstrip("/") + f"/:{parent['param']}"
        return None

    adjusted = []
    for r in routes:
        if r["kind"] not in ("resources_expanded", "resource_expanded"):
            adjusted.append(r)
            continue
        line = r["line"]
        parent = None
        for p in parents:
            if p["file"] != r["file"]:
                continue
            if p["start"] < line <= p["end"] and p["start"] != line:
                if parent is None or p["start"] > parent["start"]:
                    parent = p
        if parent is None:
            adjusted.append(r)
            continue

        # Child was expanded as /{scope}/{resource_name}[/:param...]
        # Rebuild as {parent_member_base}/{resource_name}{member_suffix}
        child_name = r.get("resource_name")
        if not child_name:
            adjusted.append(r)
            continue
        # member suffix: everything after /{child_name} in the non-nested path
        # e.g. path=/api/comments/:id → suffix=/:id ; path=/api/comments → suffix=
        marker = "/" + child_name
        idx = r["path"].rfind(marker)
        suffix = r["path"][idx + len(marker) :] if idx >= 0 else ""

        base = parent_member_base(parent, routes, r["file"])
        if not base:
            # last resort: keep original
            adjusted.append(r)
            continue
        new_path = (base.rstrip("/") + "/" + child_name + suffix).replace("//", "/")
        r = dict(r)
        r["path"] = new_path
        r["kind"] = r["kind"] + "+nested"
        adjusted.append(r)
    return adjusted


def parse_resource_name_from_path(path: str) -> str:
    parts = [p for p in path.split("/") if p and not p.startswith(":")]
    return parts[-1] if parts else ""


def scope_prefix_of(path: str, child_seg: str) -> str:
    idx = path.find("/" + child_seg)
    return path[:idx] if idx > 0 else ""


def repo_of(file_path: str) -> str:
    # /tmp/quackapi_corpus/ruby/<repo>/...
    parts = Path(file_path).parts
    try:
        i = parts.index("ruby")
        return parts[i + 1] if i + 1 < len(parts) else "unknown"
    except ValueError:
        return "unknown"


def extract_sinatra_routes(file_path: str) -> list[dict]:
    calls = sitting_duck_calls(file_path, ["get", "post", "put", "patch", "delete", "head", "options"])
    routes = []
    for c in calls:
        peek = c["peek"]
        method = c["call_name"].upper()
        path = parse_path_arg(peek, c["call_name"])
        if not path:
            continue
        if not path.startswith("/"):
            path = "/" + path
        # handler: sinatra inline block
        routes.append(
            {
                "framework": "sinatra",
                "method": method,
                "path": path,
                "handler": "sinatra#block",
                "file": file_path,
                "line": int(c["start_line"]),
                "kind": "sinatra_dsl",
                "source_dsl": peek.split("\n")[0][:200],
                "source_repo": repo_of(file_path),
            }
        )
    return routes


def extract_validations(glob_path: str, framework: str = "rails") -> list[dict]:
    """Extract validates :field, presence/numericality etc. + class name."""
    classes = sitting_duck_classes(glob_path)
    # map file -> list of (class_name, start, end)
    file_classes: dict[str, list] = {}
    for cl in classes:
        file_classes.setdefault(cl["file_path"], []).append(cl)

    calls = sitting_duck_calls(glob_path, ["validates", "validates_presence_of", "validates_numericality_of", "validates_uniqueness_of"])
    models = []
    for c in calls:
        peek = c["peek"]
        line = int(c["start_line"])
        fp = c["file_path"]
        # enclosing class
        model = "?"
        for cl in file_classes.get(fp, []):
            if int(cl["start_line"]) <= line <= int(cl["end_line"]):
                model = cl["class_name"]
        call = c["call_name"]
        if call == "validates":
            # validates :title, presence: true, numericality: ...
            fields = re.findall(r":(\w+)", peek.split("\n")[0] if "\n" not in peek[:80] else peek)
            # first symbols before keyword options are fields; options are presence, numericality, etc.
            option_keys = {
                "presence", "uniqueness", "numericality", "format", "length",
                "inclusion", "exclusion", "allow_blank", "allow_nil", "if", "unless",
                "on", "strict", "confirmation", "acceptance", "case_sensitive",
            }
            # Parse more carefully
            field_tokens = re.findall(r":(\w+)", peek)
            fields = []
            constraints = []
            required = False
            ftype = "string"  # default ActiveModel
            for tok in field_tokens:
                if tok in option_keys or tok in ("true", "false"):
                    constraints.append(tok)
                    if tok == "presence":
                        required = True
                    if tok == "numericality":
                        ftype = "number"
                else:
                    # could be field or nested option value symbol
                    if not constraints:
                        fields.append(tok)
                    else:
                        constraints.append(tok)
            # Multi-line validates :username, uniqueness: ..., presence: true
            if "presence:" in peek and re.search(r"presence:\s*true", peek):
                required = True
            if "numericality" in peek:
                ftype = "number"
            if not fields:
                # fallback first symbol
                m = re.search(r"validates\s+:(\w+)", peek)
                if m:
                    fields = [m.group(1)]
            for field in fields:
                if field in option_keys:
                    continue
                models.append(
                    {
                        "framework": framework,
                        "model": model,
                        "field": field,
                        "type": ftype,
                        "required": required,
                        "constraints": ",".join(constraints) if constraints else "",
                        "file": fp,
                        "line": line,
                        "source_dsl": peek[:300].replace("\n", " "),
                        "source_repo": repo_of(fp),
                        "kind": "activemodel_validates",
                    }
                )
        elif call == "validates_presence_of":
            for field in re.findall(r":(\w+)", peek):
                models.append(
                    {
                        "framework": framework,
                        "model": model,
                        "field": field,
                        "type": "string",
                        "required": True,
                        "constraints": "presence",
                        "file": fp,
                        "line": line,
                        "source_dsl": peek[:300],
                        "source_repo": repo_of(fp),
                        "kind": "validates_presence_of",
                    }
                )
        elif call == "validates_numericality_of":
            for field in re.findall(r":(\w+)", peek):
                models.append(
                    {
                        "framework": framework,
                        "model": model,
                        "field": field,
                        "type": "number",
                        "required": False,
                        "constraints": "numericality",
                        "file": fp,
                        "line": line,
                        "source_dsl": peek[:300],
                        "source_repo": repo_of(fp),
                        "kind": "validates_numericality_of",
                    }
                )
        elif call == "validates_uniqueness_of":
            for field in re.findall(r":(\w+)", peek):
                models.append(
                    {
                        "framework": framework,
                        "model": model,
                        "field": field,
                        "type": "string",
                        "required": False,
                        "constraints": "uniqueness",
                        "file": fp,
                        "line": line,
                        "source_dsl": peek[:300],
                        "source_repo": repo_of(fp),
                        "kind": "validates_uniqueness_of",
                    }
                )
    return models


def extract_strong_params(glob_path: str) -> list[dict]:
    """Extract params.require(:x).permit(:a, :b) as model field required flags."""
    sql = f"""
LOAD sitting_duck;
COPY (
  SELECT file_path, start_line, peek
  FROM read_ast('{glob_path}', 'ruby', peek := 'full')
  WHERE type = 'call' AND (peek LIKE '%permit%' OR peek LIKE '%require%')
  ORDER BY file_path, start_line
) TO '/tmp/quackapi_corpus/scratch/ruby_params.csv' (HEADER, DELIMITER ',');
"""
    try:
        run_sql(sql)
    except RuntimeError:
        return []
    import csv
    out = []
    with open("/tmp/quackapi_corpus/scratch/ruby_params.csv") as f:
        for r in csv.DictReader(f):
            peek = r["peek"]
            if "permit" not in peek:
                continue
            # params.require(:article).permit(:title, :body, ...)
            req = re.search(r"require\(:(\w+)\)", peek)
            model = req.group(1).capitalize() if req else "Params"
            fields = re.findall(r":(\w+)", peek)
            skip = {"require", "permit"}
            for field in fields:
                if field in skip:
                    continue
                # first field after require is the model key
                if req and field == req.group(1):
                    continue
                out.append(
                    {
                        "framework": "rails",
                        "model": model if not req else req.group(1),
                        "field": field,
                        "type": "string",
                        "required": bool(req),  # require(:x) means nested key required; fields permitted optional unless model validates
                        "constraints": "strong_params",
                        "file": r["file_path"],
                        "line": int(r["start_line"]),
                        "source_dsl": peek[:300].replace("\n", " "),
                        "source_repo": repo_of(r["file_path"]),
                        "kind": "strong_params",
                    }
                )
    return out


def write_parquet(routes: list[dict], models: list[dict]) -> None:
    """Write IR matching ir_python_routes/models.parquet column schema."""
    routes_json = CORPUS / "scratch" / "ruby_routes.jsonl"
    models_json = CORPUS / "scratch" / "ruby_models.jsonl"

    # Normalize to Python IR column names
    routes_norm = []
    for r in routes:
        routes_norm.append(
            {
                "framework": r["framework"],
                "method": r["method"],
                "path": r["path"],
                "handler_name": r.get("handler") or r.get("handler_name") or "",
                "file": r["file"],
                "start_line": int(r.get("line") or r.get("start_line") or 0),
                "repo": r.get("source_repo") or r.get("repo") or "",
                "evidence": r.get("source_dsl") or r.get("evidence") or "",
            }
        )
    models_norm = []
    for m in models:
        required = bool(m.get("required") if "required" in m else m.get("is_required"))
        models_norm.append(
            {
                "framework": m["framework"],
                "model_name": m.get("model") or m.get("model_name") or "",
                "field_name": m.get("field") or m.get("field_name") or "",
                "field_type": m.get("type") or m.get("field_type") or "string",
                "is_optional": (not required),
                "has_default": False,
                "is_required": required,
                "default_expr": None,
                "file": m["file"],
                "field_line": int(m.get("line") or m.get("field_line") or 0),
                "repo": m.get("source_repo") or m.get("repo") or "",
                "declared_annotation": m.get("source_dsl") or m.get("declared_annotation") or "",
            }
        )

    with open(routes_json, "w") as f:
        for r in routes_norm:
            f.write(json.dumps(r) + "\n")
    with open(models_json, "w") as f:
        for m in models_norm:
            f.write(json.dumps(m) + "\n")

    # Prove column alignment with Python IR
    print("Python routes columns:", end=" ")
    print(run_sql(f"SELECT string_agg(column_name, ',') FROM (DESCRIBE SELECT * FROM '{CORPUS}/ir_python_routes.parquet')").strip())

    sql = f"""
COPY (
  SELECT
    framework::VARCHAR AS framework,
    method::VARCHAR AS method,
    path::VARCHAR AS path,
    handler_name::VARCHAR AS handler_name,
    file::VARCHAR AS file,
    start_line::UINTEGER AS start_line,
    repo::VARCHAR AS repo,
    evidence::VARCHAR AS evidence
  FROM read_json_auto('{routes_json}')
) TO '{OUT_ROUTES}' (FORMAT PARQUET);

COPY (
  SELECT
    framework::VARCHAR AS framework,
    model_name::VARCHAR AS model_name,
    field_name::VARCHAR AS field_name,
    field_type::VARCHAR AS field_type,
    is_optional::BOOLEAN AS is_optional,
    has_default::BOOLEAN AS has_default,
    is_required::BOOLEAN AS is_required,
    default_expr::VARCHAR AS default_expr,
    file::VARCHAR AS file,
    field_line::UINTEGER AS field_line,
    repo::VARCHAR AS repo,
    declared_annotation::VARCHAR AS declared_annotation
  FROM read_json_auto('{models_json}')
) TO '{OUT_MODELS}' (FORMAT PARQUET);
"""
    run_sql(sql)
    print(f"Wrote {OUT_ROUTES} ({len(routes_norm)} routes)")
    print(f"Wrote {OUT_MODELS} ({len(models_norm)} model fields)")
    # Column parity check
    print(run_sql(f"""
SELECT 'routes_cols' AS what, string_agg(column_name, ',' ORDER BY column_name) AS cols
FROM (DESCRIBE SELECT * FROM '{OUT_ROUTES}')
UNION ALL
SELECT 'python_routes', string_agg(column_name, ',' ORDER BY column_name)
FROM (DESCRIBE SELECT * FROM '{CORPUS}/ir_python_routes.parquet')
UNION ALL
SELECT 'models_cols', string_agg(column_name, ',' ORDER BY column_name)
FROM (DESCRIBE SELECT * FROM '{OUT_MODELS}')
UNION ALL
SELECT 'python_models', string_agg(column_name, ',' ORDER BY column_name)
FROM (DESCRIBE SELECT * FROM '{CORPUS}/ir_python_models.parquet');
"""))



def main() -> int:
    Path("/tmp/quackapi_corpus/scratch").mkdir(parents=True, exist_ok=True)

    # Verify ruby language support claim
    verify = run_sql("""
LOAD sitting_duck;
SELECT DISTINCT language FROM read_ast('/tmp/quackapi_corpus/scratch/hello.rb', 'ruby') LIMIT 5;
""")
    print("sitting_duck language verify:", verify.strip())

    all_routes: list[dict] = []
    all_models: list[dict] = []

    # Rails realworld
    rr = RUBY / "rails-realworld/config/routes.rb"
    if rr.exists():
        all_routes.extend(extract_rails_routes(str(rr), "rails"))
        all_models.extend(extract_validations(str(RUBY / "rails-realworld/app/models/*.rb"), "rails"))
        all_models.extend(extract_strong_params(str(RUBY / "rails-realworld/app/controllers/*.rb")))

    # Canonical rails (full resources expansion demo)
    cr = RUBY / "canonical-rails/config/routes.rb"
    if cr.exists():
        all_routes.extend(extract_rails_routes(str(cr), "rails"))
        all_models.extend(extract_validations(str(RUBY / "canonical-rails/app/models/*.rb"), "rails"))

    # Canonical sinatra
    for p in [
        RUBY / "canonical-sinatra/app.rb",
        RUBY / "canonical-sinatra/simple.rb",
    ]:
        if p.exists():
            all_routes.extend(extract_sinatra_routes(str(p)))

    # Sinatra framework examples
    simple = RUBY / "sinatra-framework/examples/simple.rb"
    if simple.exists():
        all_routes.extend(extract_sinatra_routes(str(simple)))

    # Deduplicate routes
    seen = set()
    deduped = []
    for r in all_routes:
        key = (r["framework"], r["method"], r["path"], r["handler"], r["file"])
        if key in seen:
            continue
        seen.add(key)
        deduped.append(r)

    write_parquet(deduped, all_models)

    # Summary
    print("\n=== SUMMARY ===")
    by_fw: dict[str, int] = {}
    for r in deduped:
        by_fw[r["framework"]] = by_fw.get(r["framework"], 0) + 1
    print("routes by framework:", by_fw)
    print("total routes:", len(deduped))
    print("total model fields:", len(all_models))
    print("\nSample resources expansion (canonical articles):")
    for r in deduped:
        if "canonical-rails" in r["file"] and "articles" in r["path"]:
            print(f"  {r['method']:7} {r['path']:30} {r['handler']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
