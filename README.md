# Jurassic World Evolution 3 CrossOver Fix

A game-local Direct3D 12 shim for a startup crash in Jurassic World Evolution 3 under CrossOver with D3DMetal. For the crash diagnosis, how the shim works, and its limitations, see [Technical details](Docs/Technical.md).

## Install

1. Use an Intel CrossOver bottle with Steam and select **D3DMetal** as its graphics backend.
2. In Steam, open **Jurassic World Evolution 3 → Manage → Browse local files**.
3. Copy [`bin/d3d12.dll`](bin/d3d12.dll) beside `JWE3.exe`, then launch the game from Steam.

The normal Steam launch loaded the local shim on the tested setup. If CrossOver selects the system DLL first, add a `d3d12` library override for `JWE3.exe` in Wine Configuration and select **Native, then Builtin**.

To uninstall, close the game and remove the shim you copied beside `JWE3.exe`. Remove the per-game override if you added one.

## Tested setup

Steam build `25967550` on macOS 27.2, CrossOver Preview `27.0.0.41073`, and D3DMetal 4 beta 3. The game reached the menu, accepted settings changes, and loaded the first campaign level. Camera movement worked, and textures and models looked correct in that level. Longer sessions and later levels are unverified.

## Build from source

Run these commands from the project root with the x86-64 Windows target of [llvm-mingw](https://github.com/mstorsjo/llvm-mingw). The tested build used the `20261104` UCRT release.

```sh
mkdir -p bin
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -Wall -Wextra -Werror -shared -static \
  src/proxy.cpp src/proxy.def -ldxguid -luuid -o bin/d3d12.dll
x86_64-w64-mingw32-clang++ -O2 -std=c++17 -Wall -Wextra -Werror -static \
  tests/probe.cpp -ld3d12 -ldxguid -luuid -o bin/JWE3HeapProbe.exe
```

## Credits

The heap-mirroring approach comes from fladnaG86’s [FH2-Port-AppleSilicon](https://github.com/fladnaG86/FH2-Port-AppleSilicon). See the [project license](LICENSE) and [upstream MIT notice](LICENSE.fh2).
