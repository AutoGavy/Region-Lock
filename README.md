# Region Lock Native

[![License: AGPL-3.0](https://img.shields.io/badge/License-AGPL--3.0-blue.svg)](LICENSE)

A C++20 / Win32 region selection tool for Battlefield 6, with an English interface and a dark main window. It uses Windows Firewall to block the **selected game executable** from reaching UDP ports `21000-21299` in unselected AWS EC2 regions, which can influence regional discovery and matchmaking.

The AWS catalog is not a Battlefield 6 server list, and selecting a region is not a server-side guarantee. Ports, discovery behavior, and server redirects can change. Restart the game after changing the lock; unlock first if joining friends across regions fails. Real Battlefield 6 matchmaking acceptance has not been completed.

## Use

1. Run `dist/RegionLock.exe` and accept the administrator prompt. Windows Firewall and Base Filtering Engine must be running.
2. Choose the actual Battlefield 6 game EXE. Rules use its full path and apply only to the matching traffic from that executable.
3. Check the regions to keep, then apply the selection. The corresponding address ranges of unchecked regions are blocked. Use the unlock action to remove the restriction.
4. Minimize to hide the window in the notification area. Restore it from the tray icon or menu. The system-menu and tray exit actions are labeled **Unlock regions and exit**. The window's close button, Alt+F4, and the tray exit action all remove this tool's lock rules before exiting. If cleanup fails, the window remains open and reports the error so you can retry.
5. Rules remain active while minimized. After an abnormal termination, the next startup removes leftover rules belonging to this tool.

Only one instance runs in the current Windows session, including when launched from another copy of the EXE. Launching again restores and activates the existing window, including from the tray. While an update or another background task is running, the region list remains dark and scrollable, with checkbox changes locked until the task finishes.

Each new launch centers the window in the work area of the monitor containing the mouse pointer, with DPI scaling. Restoring from the tray preserves the window's position.

Applying and removing rules run in background tasks, and the interface reports the result. Settings and the catalog cache are stored in `%LOCALAPPDATA%\RegionLockNative`. On first use, the program can load `ip-ranges.json` beside the EXE. Updating downloads the official AWS catalog over HTTPS and replaces the cache only after validation; a failed download or validation leaves the existing cache intact.

If a required firewall service is stopped, the program reports the cause without starting the service or changing its startup type. Cleanup only handles entries in the `RegionLock.Native.v1` group with this tool's rule-name prefix. It does not remove rules from the reference tool or other applications. If you previously used another region lock tool, unlock through that tool first; its remaining rules can still affect the connection.

## Behavior and safeguards

- Rules are scoped to the game EXE rather than all applications.
- Only entries whose service is exactly `EC2` are accepted. JSON, region codes, and CIDR ranges are validated; both IPv4 and IPv6 are supported, and regions are listed dynamically.
- Address ranges are normalized, deduplicated, and merged without changing their coverage. Rules are split into bounded batches. If selected and blocked regions overlap, applying is rejected to avoid blocking a selected region.
- A complete new generation of rules is installed before the old generation is removed. Failures trigger an attempted rollback and a reported result. Windows Firewall COM provides no transaction across multiple rules, so both generations can briefly apply during a change.
- Catalog and settings replacements are validated before installation. Downloads and files have size limits, and network operations have timeouts.
- Network tests show successful replies and the median latency of successful samples.

The network test sends five UDP probes to the AWS GameLift beacon at `gamelift-ping.<region>.api.aws:7770`, limits the send rate, and validates replies. These results are a reference for the AWS beacon, **not live game-server latency**, and do not prove that region locking works. Some EC2 regions have no corresponding beacon; unavailable tests report the reason.

## Game accelerators

The rules target the chosen game executable and unselected AWS address ranges on UDP ports `21000-21299`. Region Lock does not change an accelerator's process, nodes, proxy settings, or routes. The game's connection attempts can still be blocked, so region selection and the accelerator's target region should agree. Joining friends or falling back to a different region may require unlocking first.

Compatibility depends on how the accelerator redirects or tunnels game traffic. It can affect which application identity and destination the firewall evaluates, so a particular accelerator may weaken the lock or conflict with matchmaking. This is an implementation-dependent inference, not a verified result for any specific accelerator. For comparison, keep the same accelerator and node, unlock, restart the game, and compare the connection behavior. [Microsoft's application-layer filtering documentation](https://learn.microsoft.com/en-us/windows/win32/fwp/application-layer-enforcement--ale-) explains application-identity filtering.

## Performance

The native Win32 message loop waits for system messages when idle. There is no continuous drawing, per-frame scanning, or periodic network polling, including while hidden in the tray. Downloads, probes, and firewall operations start background work only when needed; exit cancels and joins outstanding work.

Release builds target x64 with the static C/C++ runtime, `/O2`, `/GL` + `/LTCG`, and unused function/data removal and folding. CFG, ASLR, and DEP remain enabled. AVX2 is not required. The program does not change the system power plan, request realtime process priority, or adjust global network settings.

The latest local UI smoke measurements are recorded in `artifacts/smoke.json`. Short idle samples depend on scheduler and timer resolution: a measured CPU-time delta of zero does not mean zero work on every machine. These samples do not include network or firewall operations and are not a general performance guarantee.

## Build and validation

Requires Windows 10/11 x64, Visual Studio 2022 or 2026 with Desktop development with C++, and CMake supporting the installed Visual Studio generator (project minimum: 3.28). Dependencies are included in the source; building does not download libraries.

```powershell
.\scripts\Build.ps1
```

The script locates Visual Studio and CMake, configures x64 Release, builds, and runs core and backend tests. A successful build produces `dist/RegionLock.exe`, the catalog, documentation, and licenses. Packaging and offline catalog tests use `assets/ip-ranges.json`. The current bundled official snapshot was created at `2026-10-31-08-47-05` and contains 43 EC2 regions, 1,898 IPv4 prefixes, and 2,579 IPv6 prefixes before compaction.

If `RegionLock.exe` is running, the script stops the build or deployment instead of terminating the process. Options include `-Configuration Debug`, `-NoDeploy`, and `-SkipTests`; tests run by default.

Tests cover JSON/CIDR parsing, address normalization and compaction, region overlap detection, rule scope, catalog fixtures, settings and file replacement, and network cancellation before work begins. Backend tests use temporary files and do not modify the firewall or send network probes. `RegionLockSmoke.exe` is a dedicated UI test build without elevation. It skips real firewall changes and user settings, using local catalog data to check the window, tray minimize/restore, close behavior, and sampled idle overhead:

```powershell
.\build\Release\RegionLockSmoke.exe --smoke-test --report C:\Temp\region-lock-smoke.json --screenshot C:\Temp\region-lock-smoke.png
```

Use an existing writable output directory. Core, backend, and UI smoke results do not substitute for actual firewall/network testing or real Battlefield 6 matchmaking acceptance.

An optional live network check is available separately:

```powershell
.\build\Release\PlatformTests.exe --network
```

This first runs the offline backend tests, then downloads and parses the official AWS catalog over HTTPS and sends five UDP probes to the Tokyo GameLift beacon, reporting reply count and median latency. It does not write the user cache or change the firewall. This option is excluded from default CTest and depends on the current network connection; probe errors are reported without automatic retries. The local live check successfully downloaded the catalog; the Tokyo UDP beacon returned 0/5 replies. That result does not establish whether game traffic or region locking works.

## References and third-party components

Inspired by Yuyicheng / Dogfight360's [Battlefield 6 matchmaking region lock tool Ver 1.2](https://www.dogfight360.com/blog/18892/). This project is an independent C++ implementation and does not use the original tool's executable or decompiled code as a runtime dependency.

The local `Samples` folder, including the original tool's executable, is excluded from this repository. Builds and tests do not depend on it.

- [AWS IP address range documentation](https://docs.aws.amazon.com/vpc/latest/userguide/aws-ip-ranges.html) and [official JSON catalog](https://ip-ranges.amazonaws.com/ip-ranges.json): categorized by AWS region and service, but not an inventory of every AWS service or address source. Local Zone locations can differ from their parent region.
- [Amazon GameLift UDP ping beacons](https://docs.aws.amazon.com/gameliftservers/latest/developerguide/reference-udp-ping-beacons.html): endpoints, protocol, and rate limits.
- [Windows Firewall INetFwRule](https://learn.microsoft.com/windows/win32/api/netfw/nn-netfw-inetfwrule): rule scoping by application, protocol, port, and remote address.
- JSON parser: [yyjson v0.13.0](https://github.com/ibireme/yyjson/tree/0.13.0), MIT license. Sources and SHA-256 checksums are in [third_party/README.md](third_party/README.md); the license is in [third_party/yyjson/LICENSE](third_party/yyjson/LICENSE).

## License

Copyright (C) 2026 AutoGavy.

Region Lock Native is licensed under the **GNU Affero General Public License, version 3 only** (`AGPL-3.0-only`). You may redistribute and modify it under that license. It is provided without any warranty, including the implied warranties of merchantability or fitness for a particular purpose. See [LICENSE](LICENSE) for the full terms.

Corresponding source is available at [AutoGavy/Region-Lock](https://github.com/AutoGavy/Region-Lock). The build script includes this license and the third-party license in the generated package. Vendored yyjson retains its MIT license; the AWS catalog files retain their upstream provenance.
