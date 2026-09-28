# quackapi — agent notes

Before every commit that touches `src/` or `test/`, run `make format-ci`. It formats with the
exact tools the Code Quality check uses (clang-format 11.0.1, black 24, via uv); `make format-fix`
uses whatever clang-format is on PATH, which formats differently or is missing, and a
format-only red CI run is the result. `make format-ci-check` is the check CI runs.
