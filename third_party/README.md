# Third-party sources

`yyjson/` contains the unmodified JSON parser from the official
[ibireme/yyjson v0.13.0 tag](https://github.com/ibireme/yyjson/tree/0.13.0).
It is vendored locally; builds do not download dependencies or need a package manager.
The upstream MIT license is in `yyjson/LICENSE`.

Source URLs and SHA-256 checksums:

| File | URL | SHA-256 |
| --- | --- | --- |
| yyjson.c | https://raw.githubusercontent.com/ibireme/yyjson/0.13.0/src/yyjson.c | d2d58ef0a3b2267862dc363832c4f3185fd375aea933d5d3419d1a940d6ece2e |
| yyjson.h | https://raw.githubusercontent.com/ibireme/yyjson/0.13.0/src/yyjson.h | ef803cda5c06b8962face6dfa39c3284b3bcf3e73f4d7317664690cc779679ca |
| LICENSE | https://raw.githubusercontent.com/ibireme/yyjson/0.13.0/LICENSE | 45e384d3d52c73cba3a64d6e6c25d47cd738cd8a55c30629e3201046eda62947 |

The catalog parser uses yyjson's strict default mode: comments, trailing commas,
non-finite numbers, invalid UTF-8, and trailing non-whitespace data are rejected.
The application additionally validates AWS's schema and duplicate object keys.
