# Changelog

All notable changes to this project are documented here. Each version
links to its full release notes on GitHub.

The format is based on [Keep a Changelog](https://keepachangelog.com/).

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
