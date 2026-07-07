# Third-party notices

The OpenVPN Client ACAP is distributed under AGPL-3.0-or-later (see `LICENSE`).
It bundles or links the following third-party components, each under its own
license. This file collects their attributions.

## OpenVPN3 core (`lib/tun_probe`)
- Project: <https://github.com/OpenVPN/openvpn3>
- Copyright (C) 2012- OpenVPN Inc.
- License: **AGPL-3.0-only WITH openvpn3-openssl-exception** (also available under
  a separate commercial license from OpenVPN Inc.). The OpenSSL exception permits
  linking against OpenSSL.
- Compiled from source and shipped as `lib/tun_probe`.

## asio (header-only, compiled into `lib/tun_probe`)
- Project: <https://github.com/chriskohlhoff/asio>
- Copyright (C) Christopher M. Kohlhoff
- License: **Boost Software License 1.0 (BSL-1.0)**

## LZ4 (compiled into `lib/tun_probe`)
- Project: <https://github.com/lz4/lz4>
- Copyright (C) Yann Collet
- License: **BSD 2-Clause**

## OpenSSL (linked, provided by Axis OS)
- Project: <https://www.openssl.org/>
- License: **Apache License 2.0** (OpenSSL 3.x)

## Go netstack sidecar (`lib/netstack_proxy`)
Statically linked Go binary built from:
- **gVisor** (`gvisor.dev/gvisor`) — Copyright The gVisor Authors — **Apache-2.0**
- **wireguard-go** (`golang.zx2c4.com/wireguard`, `tun/netstack`) —
  Copyright (C) Jason A. Donenfeld — **MIT**
- **golang.org/x/{crypto,net,sys,time}** — Copyright The Go Authors —
  **BSD 3-Clause**
- **github.com/google/btree** — Copyright Google — **Apache-2.0**

## Trademarks
"OpenVPN" is a registered trademark of OpenVPN Inc. "WireGuard" is a registered
trademark of Jason A. Donenfeld. This is an independent, community project and is
not affiliated with, endorsed by, or supported by OpenVPN Inc., Axis
Communications AB, or the WireGuard project.
