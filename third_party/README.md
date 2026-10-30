# third_party

Code taken from other projects and kept unmodified. Licence texts and credits: `THIRD_PARTY_NOTICES.md` at the
repository root.

Origin of everything here: the DOA2U netplay tree, `C:/Users/fsop1/doa2u-release` (branch `netplay-release`),
commit `6c35c7b`, folder `doa2u/third_party/`. Each copy was checked byte for byte against that tree (`diff -r`).

| Folder | What | Licence | Upstream |
|---|---|---|---|
| `GekkoNet/` | lockstep input exchange (sessions, input delay, desync detection) | BSD 2-Clause, (c) 2024-2026 Jamie Meyer | https://github.com/HeatXD/GekkoNet |
| `GekkoNet/thirdparty/zpp/` | `zpp_bits`, the serialiser GekkoNet uses | MIT, (c) 2021 Eyal Z | https://github.com/eyalz800/zpp_bits |
| `miniupnpc/` | UPnP client, opens the host's UDP port on the router (version 2.3.3, `VERSION`) | BSD 3-Clause, (c) 2005-2025 Thomas Bernard | https://github.com/miniupnp/miniupnp |

## What was copied

- `GekkoNet/`: `LICENSE`, `README.md`, `include/**`, `src/*.cpp`, `thirdparty/zpp/**`. Not copied:
  `thirdparty/asio` (the library's own UDP adapter). Only `src/gekkonet.cpp` (lines 137-259) and
  `include/gekkonet.h` (lines 252-261) mention it, both inside `#ifndef GEKKONET_NO_ASIO`; the build defines
  `GEKKONET_NO_ASIO`, and the port's own transport carries the packets through a `GekkoNetAdapter`.
- `miniupnpc/`: the whole folder. Only the 14 sources listed in `CMakeLists.txt` are built (the list of
  DOA2U's `src/online/CMakeLists.txt`). The other 11 `.c` files in `miniupnpc/src` are not needed: ten are
  command-line tools and test programs with their own `main` (`upnpc.c`, `listdevices.c`, `minihttptestserver.c`,
  `minixmlvalid.c`, and the six `test*.c`), and `miniupnpcmodule.c` is the Python module.

## How it is built

`CMakeLists.txt` defines the static libraries `gekkonet` (`GEKKONET_STATIC GEKKONET_NO_ASIO`, C++20) and
`miniupnpc` (`MINIUPNP_STATICLIB`) and links both into `dead_or_alive_4`.

## Updating

Do not edit these files. To update, copy them again from the newer upstream or DOA2U commit, change the commit
above, and run `tests\host\run_np_gekko_test.bat`: it pins the GekkoNet behaviour the netplay session executor
relies on (event order, one input per frame, desync detection).
