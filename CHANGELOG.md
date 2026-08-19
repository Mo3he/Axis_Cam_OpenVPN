# Changelog

All notable changes to this project are documented here. Each version
links to its full release notes on GitHub.

The format is based on [Keep a Changelog](https://keepachangelog.com/).

## [0.1.4] - 2026-08-19 - Fix the userspace data plane, broken since 0.1.2

- Fix: 0.1.2 renamed the app from `OpenVPN` to `OpenVPN_VPN` but did not update
  the sidecar path in the tun probe, so it still launched the netstack sidecar
  from `/usr/local/packages/OpenVPN/lib`. The `execl` failed on every connect,
  so from 0.1.2 onward the tunnel came up and reported connected while carrying
  no traffic: the transparent forwarders, the inbound SOCKS5 proxy and the
  outbound proxies never ran, and the client logged
  `TUN write exception: Connection refused`. 0.1.0 and 0.1.1 were unaffected.
- A failed sidecar launch is now logged instead of exiting silently.
- The probe and sidecar log as `OpenVPN_VPN`, so their output appears in the
  app log alongside the bridge instead of under a separate `OpenVPN` tag, which
  is why the failure left no visible trace.

## [0.1.3] - 2026-08-19 - Configurable forwarded ports

- The directly forwarded ports are now configurable instead of fixed at 80, 443,
  and 554. Add port 22 to reach SSH over the VPN, or any other camera service.
  Set them under **Forwarded ports** in Settings, or through the `ForwardPorts`
  parameter. Up to 16 ports; clearing the field restores the default.

## [0.1.2] - 2026-07-24 - Fix broken app name and save settings without param.cgi

- Fix: the app package name in the bridge and web UI did not match the manifest
  `appName` (`OpenVPN_VPN`). As a result the client binary was launched from the
  wrong path (the tunnel never started) and settings were read from the wrong
  parameter namespace. All internal references now use `OpenVPN_VPN`.
- Fix: settings (username, password, proxy ports) can now be saved on Axis
  devices that do not expose `/axis-cgi/param.cgi`, such as recorder/NVR
  products (e.g. S3008) and access-control controllers (e.g. A1610, A1710,
  A1810).
- The app's existing HTTP endpoint now also serves
  `/local/OpenVPN_VPN/api/settings`. The web UI uses `param.cgi` when available
  and transparently falls back to this endpoint when it is not, writing settings
  through the ACAP parameter store. The embedded server moved to port 2204 to
  avoid clashing with the other VPN ACAPs' settings servers.

## [0.1.1-Signed] - 2026-07-21 - OpenVPN VPN 0.1.1 (Signed)

- Packages are now signed with the Axis ACAP signing service and install
  normally on AXIS OS 12.10 and later.
- Vendor updated to `moshe@mohome.net` with the registered vendor ID.
- App renamed from "OpenVPN Client" to "OpenVPN VPN" (`appName` `OpenVPN_VPN`).
- Upgrading from an earlier unsigned version can fail with "Couldn't
  install: app" (device log: "Vendor ID in manifest does not match the
  vendor ID of the previous version"). Back up your config, uninstall the
  old version, then install this one.

## [0.1.1] - 2026-07-07 - OpenVPN Client v0.1.1

## [0.1.0] - 2026-07-06 - OpenVPN Client v0.1.0

[0.1.1]: https://github.com/Mo3he/Axis_Cam_OpenVPN/releases/tag/v0.1.1
[0.1.0]: https://github.com/Mo3he/Axis_Cam_OpenVPN/releases/tag/v0.1.0
