# Region Lock

Native Windows region selection for Battlefield 6. English interface, dark theme.

**[Download](https://github.com/AutoGavy/Region-Lock/releases/latest)** — Windows 10/11 x64. Administrator permission and Windows Firewall required.

## Use

1. Extract the ZIP and keep `ip-ranges.json` beside `RegionLock.exe`.
2. Run the EXE, select `bf6.exe`, check allowed regions, and click **Apply lock**.
3. Restart the game after changing regions.

Click a region row, then **Test region** to test it, or choose **Test all**.

The tray icon stays available while running. Minimize to keep the lock active with low resource priority. Close to **unlock regions and exit**.

**Update data** refreshes the official AWS IP ranges. Rules affect only the selected game. AWS ranges are not an official Battlefield server list; matchmaking results are not guaranteed.

## Build

Visual Studio 2022/2026 with C++ and CMake 3.28+:

```powershell
.\scripts\Build.ps1
```

Output: `dist/RegionLock.exe`. Tests run by default.

## License

[AGPL-3.0-only](LICENSE) · Copyright (C) 2026 AutoGavy · No warranty.
[yyjson](third_party/README.md) retains its [MIT license](third_party/yyjson/LICENSE).
