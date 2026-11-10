# cpp-httplib dependency

Vendored unmodified from [yhirose/cpp-httplib v0.65.0](https://github.com/yhirose/cpp-httplib/releases/tag/v0.65.0).
Tag commit: `7e5d0683ac326788e52ce3c44585c1af24b04532`.
License: MIT; full original terms in [LICENSE](LICENSE).

Files and SHA256:

- `httplib.h`: `17d85dcf129d3cc67eefba846608016f43882c9de7fae811a930f9736a10eeaa`
- `LICENSE`: `4b45cbe16d7b71b89ae6127e26e0d90a029198ca5e958ad8e3d0b8bbed364d8b`

Original source: `https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.65.0/httplib.h`.
No network download occurs during CMake configuration/build. HTTP only; TLS and
compression integrations are not enabled. Include this header only in the HTTP
implementation; avoid leaking HTTP-library types into engine/task interfaces.

EdgeInspect uses the streaming multipart ContentReader API, explicitly limits
parts/image/asset/body sizes, and writes request-owned temporary files. The fixed
HTTP thread pool is separate from the single inference worker. See
[upstream documentation at the pinned version](https://github.com/yhirose/cpp-httplib/blob/v0.65.0/README.md).
