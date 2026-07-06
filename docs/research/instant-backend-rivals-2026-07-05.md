# Instant-backend rivals — the 10M-ceiling category study

**Date:** 2026-07-05 (research agent sweep; condensed digest of the full report — adoption
numbers from GitHub/HN as of this date.)

**Question answered:** what category does a "10 million people could use this" backend artifact
live in, who owns it, what exactly do the winners ship, and is the DuckDB lane taken?

## Category: instant backend / backend-in-a-box

Frameworks are chosen by language communities; instant backends are chosen by anyone with an app
idea. The single-artifact king is PocketBase; the hosted king is Supabase.

## PocketBase (59.4k★, perpetual pre-1.0, bus-factor 1)

**Feature list (the parity target):** SQLite WAL + view collections · CRUD API with
filter/sort/expand · auth: email/password + **15 OAuth2 providers** + OTP/MFA · per-collection
API rules (an RLS analog written as filter expressions) · SSE realtime subscriptions · file
storage (local/S3) + thumbnails · **admin dashboard baked into the binary** · extensibility as a
Go library or embedded goja JS-VM hooks (`pb_hooks/`, onRecordCreate, custom routes, cron) ·
JS/Go migrations auto-generated from admin-UI changes · backup/restore + Litestream companion for
durability · auto-TLS · ~15MB binary · official JS + Dart SDKs.

**Adoption drivers ranked (from Show HN 563pts / HN 630pts threads):**
1. ONE file, zero deps
2. Firebase-refugee framing ("open source Firebase alternative in 1 file")
3. Admin UI = 60-second demo-ability — the single biggest wow factor in comments
4. SQLite-as-a-feature (not apologized for)
5. Supabase self-host pain as a growth engine
6. Extensibility without losing the single artifact
7. Responsive solo maintainer

**NOT drivers:** performance, scale, enterprise features.

**Complaints:** no horizontal scaling · SQLite write ceiling · bus factor 1 · no official cloud ·
missing bulk insert / raw SQL access / CSV import / any analytics story.

## Supabase

Draw: Postgres-is-the-product, PostgREST instant REST, RLS ("impossible to bypass"), GoTrue
auth, realtime CDC. **Self-hosting is the category's biggest documented pain** — 10+ containers,
Logflare CPU hog, nerve-racking upgrades (discussion #39820). Every self-host complaint is a
PocketBase (and quackapi) customer.

## PostgREST / Hasura

Beloved for schema reflection, DB-native RLS, zero-privilege API server — but capped by the
business-logic escape-hatch problem (logic always ends up in a second service) and by being
components, not products.

## Datasette — the cautionary tale

Genius engine, but read-only core + no auth + "data publishing" framing = niche forever.
**Writes + auth + app-backend framing turns the same idea into PocketBase-scale adoption.**
quackapi must not repeat this: the framing is "build your app's backend," not "publish data."

## The DuckDB lane is EMPTY (verified)

- community `httpserver` ext: experimental query-over-HTTP endpoint — no routing, no framework
- official Quack protocol (May 2026, port 9494): DuckDB↔DuckDB infrastructure — a thing to RIDE
  (multi-writer story), not a competitor
- `duckdb_featureserv`: geo-only

Nobody has built the instant backend on the fastest-growing database of the decade.

## The category-ceiling checklist

**MUST (every category member ships these — never compromise):**
1. Instant CRUD for any table with filter/sort/paginate/expand
2. Auth: email/password + ≥5 OAuth providers + reset/verify flows + JWT issuance
3. Declarative row-level policies
4. Admin dashboard served from the artifact itself
5. SSE realtime subscriptions
6. File storage (local + S3)
7. Versioned migrations, auto-diffed
8. Backup one-liner + replication/durability story
9. Typed JS/TS SDK generated from our OpenAPI
10. Auto-TLS, logs, health out of the box
11. Single-artifact zero-config boot

**SHOULD (most have):**
12. JS-VM-or-webhook escape hatch · 13. cron scheduling · 14. SMTP + templated auth emails ·
15. bulk CSV/Parquet import/export · 16. Dart SDK · 17. rate limiting · 18. multi-writer (via
Quack protocol for us) · 19. Litestream-analog durability doc

**DIFFERENTIATOR (nobody in the category has ANY):**
20. Analytics routes — OLAP dashboards/cohorts/funnels zero-ETL ("your admin UI includes a
warehouse") · 21. lakehouse routes over Parquet/Iceberg/S3 via httpfs · 22. zero-copy
pandas/polars access to the served file · 23. everything-is-SQL introspectable catalog ·
24. columnar performance marketing · 25. in-DB FTS + vector search ("Algolia + pgvector
included") · 26. time-travel/audit reads

## Exploitable openings, ranked

1. PocketBase bus-factor / pre-1.0 fatigue
2. Zero analytics story anywhere in the category
3. SQLite write ceiling
4. Supabase self-host pain

**Positioning line:** *"PocketBase, but your database is a warehouse."*
