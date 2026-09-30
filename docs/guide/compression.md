# Response compression

`quackapi_serve` negotiates response compression from `Accept-Encoding` for
compressible response bodies.

```sql
SELECT * FROM quackapi_serve(
  8000,
  compression := 'auto',          -- auto | gzip | zstd | off
  compression_min_bytes := 1024
);
```

In `auto` mode, the server chooses the accepted coding with the highest q-value
and prefers zstd when gzip and zstd have the same q-value. `gzip` and `zstd`
restrict the available coding while still honoring the client’s header; `off`
leaves responses unchanged. The default is `auto`, preserving the previous
default-on negotiation behavior. Boolean values remain accepted for compatibility:
`true` means `auto` and `false` means `off`.

Bodies smaller than 1024 bytes by default, Server-Sent Events
(`text/event-stream`), and responses that already have `Content-Encoding` are
never compressed. Eligible responses include `Vary: Accept-Encoding`, and the
wire `Content-Length` describes the final body.
