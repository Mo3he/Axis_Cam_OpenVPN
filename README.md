# OpenVPN ACAP for Axis Cameras

[![Release](https://img.shields.io/github/v/release/Mo3he/Axis_Cam_OpenVPN?style=flat)](https://github.com/Mo3he/Axis_Cam_OpenVPN/releases)
[![License](https://img.shields.io/github/license/Mo3he/Axis_Cam_OpenVPN?style=flat)](LICENSE)
[![Build](https://github.com/Mo3he/Axis_Cam_OpenVPN/actions/workflows/build.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_OpenVPN/actions/workflows/build.yml)
[![Super-Linter](https://github.com/Mo3he/Axis_Cam_OpenVPN/actions/workflows/super-linter.yml/badge.svg)](https://github.com/Mo3he/Axis_Cam_OpenVPN/actions/workflows/super-linter.yml)
[![Sponsor](https://img.shields.io/badge/Sponsor%20My%20Work-EA4AAA?style=flat&logo=github&logoColor=white)](https://github.com/sponsors/Mo3he)
[![Buy Me A Coffee](https://img.shields.io/badge/Buy%20Me%20A%20Coffee-FFDD00?style=flat&logo=buy-me-a-coffee&logoColor=black)](https://www.buymeacoffee.com/mo3he)

An **OpenVPN client that runs directly on Axis cameras** as an ACAP application,
entirely in **userspace** with **no root** and no kernel TUN device. It makes the
camera reachable from your OpenVPN network and lets the camera route its own
traffic out through the tunnel.

> **Disclaimer:** Independent, community-developed ACAP package. Not an official
> Axis product and not affiliated with, endorsed by, or supported by Axis
> Communications AB or the OpenVPN project. Use at your own risk.

## Overview

An OpenVPN client packaged as an ACAP, running fully in userspace so it needs no
special privileges on the camera.

- **No root required:** runs as the standard unprivileged `sdk` ACAP user.
- **Works on AXIS OS 12** (and 11.x), verified on AXIS OS 12.10.
- **No kernel TUN / CAP_NET_ADMIN:** the OpenVPN3 core terminates the tunnel in
  userspace and hands packets to an in-process gVisor netstack.

## Compatibility

- **AXIS OS:** 11.x through 13.
- **Architectures:** `aarch64` and `armv7hf`.

## Installation

> **Signed packages:** Release `.eap` files are signed with the Axis ACAP
> signing service and install normally on AXIS OS 12.10 and later.
>
> **Upgrading from an earlier version?** The signing vendor changed, so
> installing over a previously installed unsigned build can fail with
> **"Couldn't install: app"** (device log: *"Vendor ID in manifest does not
> match the vendor ID of the previous version"*). To upgrade: back up your app
> configuration, **uninstall** the old version, then install the signed one.

Download the `.eap` for your camera's architecture and install via the camera
web interface under **Apps -> Add app**.

| Architecture | File |
|---|---|
| aarch64 | `OpenVPN_<version>_aarch64.eap` |
| armv7hf | `OpenVPN_<version>_armv7hf.eap` |

## Configuration

Open the app's settings page and:

1. Paste your `.ovpn` profile (inline certs supported) or pick a file.
2. Add **Username/Password** only if your profile is not autologin.
3. Optionally change the outbound proxy ports (defaults 8080 / 1080) to avoid
   clashing with other VPN ACAPs.
4. **Save & Restart**.

The status page shows the connection state, the assigned **VPN IP**, and the
proxy addresses.

### Reaching the camera from the VPN

Once connected, from any machine on the OpenVPN network:

```sh
curl http://<vpn-ip>/           # camera web UI via the tunnel
curl -k https://<vpn-ip>/       # HTTPS
# RTSP: rtsp://<vpn-ip>:554/...
# Any other port: use the inbound SOCKS5 proxy at <vpn-ip>:1080
```

### Routing camera traffic through the VPN

Set the camera's **System -> Network -> Global proxy** (HTTP/HTTPS) to
`http://127.0.0.1:8080`, or point a SOCKS5-aware service at `127.0.0.1:1080`.

## Ports & security

| Service | Address | Purpose |
|---|---|---|
| Transparent forwarders | `<vpn-ip>:80 / 443 / 554` | Camera web UI, HTTPS, and RTSP over the tunnel |
| Inbound SOCKS5 | `<vpn-ip>:1080` | Reach any camera port from the VPN |
| Outbound HTTP CONNECT | `127.0.0.1:8080` | Camera -> VPN/internet |
| Outbound SOCKS5 | `127.0.0.1:1080` | Camera -> VPN/internet for SOCKS5-aware apps |

> **Security:** the inbound SOCKS5 proxy is bound to the tunnel IP and is
> therefore reachable by any peer on your OpenVPN network. Restrict access with
> your VPN's own ACLs, and keep the camera behind its normal authentication.

## How it works

Three layers, the same pattern as the WireGuard and ZeroTier camera ACAPs:

1. **OpenVPN3 core** (`lib/tun_probe`, C++) connects to your server. Built with
   `USE_TUN_BUILDER`, its `tun_builder_establish()` returns one end of a
   `SOCK_DGRAM` socketpair instead of opening `/dev/net/tun`, so no privileges
   are needed. The encrypted transport is an ordinary UDP/TCP socket.
2. **Userspace netstack** (`lib/netstack_proxy`, Go) attaches a
   [gVisor](https://gvisor.dev/) netstack to that socketpair and runs the proxy
   layer:
   - Transparent TCP forwarders for camera ports **80 / 443 / 554**
   - **Inbound SOCKS5** on `<vpn-ip>:1080` (reach any camera port from the VPN)
   - **Outbound HTTP CONNECT** on `127.0.0.1:8080` (camera -> VPN/internet)
   - **Outbound SOCKS5** on `127.0.0.1:1080`
3. **C bridge** (`OpenVPN`) reads settings via axparameter, serves a small
   HTTP endpoint for uploading the `.ovpn` profile (too large for the parameter
   store), launches the client, and restarts it on config changes with a watchdog.

## Build from source

Requires Docker or Podman.

```sh
./build.sh                 # aarch64 + armv7hf
ARCHES=aarch64 ./build.sh  # single arch
```

The build is two-staged: `Dockerfile.openvpn3` cross-compiles the OpenVPN3 core
(cached base image), then `Dockerfile` builds the Go sidecar, the C bridge, and
packages the `.eap`.

## Limitations

Like the non-root WireGuard/ZeroTier builds, this is a userspace proxy data
plane: the camera is reachable from the VPN and can route out through it, but the
whole camera OS is not transparently placed on the VPN. Server-pushed DNS and
`redirect-gateway` apply to the netstack, not the camera's system resolver.

## Links

- [OpenVPN](https://openvpn.net/)
- [OpenVPN3](https://github.com/OpenVPN/openvpn3)
- [Axis Communications](https://www.axis.com/)

## License

This project is licensed under the **GNU Affero General Public License v3.0**
(AGPL-3.0-or-later); see [LICENSE](LICENSE). AGPL is required because the app
bundles the [OpenVPN3](https://github.com/OpenVPN/openvpn3) core, which is
AGPL-3.0-or-later.

Third-party components and their licenses are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
