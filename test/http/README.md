# removed

HTTP behavior is tested via **SQLLogic** + `quackapi_request` in `test/sql/`.

Shell scripts (curl + ports) are not the community extension path and are not run by CI.

```bash
./build/release/test/unittest "test/sql/quackapi*"
```
