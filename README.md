# Jurassic World Evolution 3 CrossOver Fix

JWE3 1.5.6 needs two fixes on the tested CrossOver Preview setup. MacGameFix's Wine module emulates the execution breakpoint the game expects under Rosetta. The game-local D3D12 shim then handles a heap operation that D3DMetal does not implement. The shim is the same DLL used for the earlier game build. See [Technical details](Docs/Technical.md) for the original graphics crash and shim design.

## Install for JWE3 1.5.6

1. Use CrossOver Preview **27.0.0.41073 (20261015)** with an Intel Windows bottle and **D3DMetal 4 beta 3**. Quit Steam and the other Windows apps running in Preview, then quit Preview. Keep a copy of the original CrossOver Preview app if you want to restore its original signature without reinstalling.
2. Get MacGameFix from its [GitHub repository](https://github.com/MathiasKowoll/MacGameFix) and open the app. Under **Improvements → Copy protection under Rosetta**, enable the switch for **CrossOver Preview 20261015 (27.0.0.41073)**. MacGameFix replaces the matching Intel Wine module and re-signs the CrossOver Preview app. [MacGameFix installation details](https://github.com/MathiasKowoll/MacGameFix/blob/main/engine/README.md#installing-them)
3. Reopen CrossOver Preview and start Steam in the Intel bottle. In JWE3's Steam library entry, select **Manage → Browse local files** and copy [`bin/d3d12.dll`](bin/d3d12.dll) beside `JWE3.exe`. Back up any existing game-local `d3d12.dll` before replacing it.
4. Click **Play** in Steam. If the game loads the system D3D12 DLL first, open the bottle's **Wine Configuration**, add `JWE3.exe` under **Applications**, and set the `d3d12` override under **Libraries** to **Native, then Builtin** for that executable.

## Collect logs and report a problem

1. Quit JWE3 and fully exit Steam using **Steam → Exit**. In CrossOver Preview, select the bottle that contains JWE3, select its **Steam** launcher, and click **Run with Options**.
2. Enable **Create log file** and choose a location for the CrossOver `.cxlog`, such as your Desktop. Enter `+loaddll,+seh` under **Wine logging channels**. Add this line under **Environment variables**:

   ```text
   JWE3_D3D12_LOG=C:/JWE3-d3d12.log
   ```

3. Click **Run**, then use **Play** in the Steam window that opens. Reproduce the problem once. Afterward, close the game and Steam to finish capturing the CrossOver log. [CodeWeavers logging instructions](https://www.codeweavers.com/support/docs/crossover-mac/index#bottle-view)
4. In CrossOver, select the same bottle and choose **Bottle Actions → Open C: Drive**. Copy `JWE3-d3d12.log` from the root of that drive before retrying. The shim replaces the file when it starts logging again.

The shim log can show whether it loaded the system D3D12 backend, hooked the device, or opened a mirrored heap. If no shim log appears, include the `.cxlog` anyway and mention that the shim log was missing. The CrossOver log can show which D3D12 DLL was loaded or whether the game failed before reaching the shim.

Open an issue and attach the shim log and CrossOver log from the same run. In Finder, use **Compress** to zip the `.cxlog` for upload. Check the logs for personal details such as your macOS username in file paths before attaching them to the public issue. [Report a problem on GitHub](https://github.com/StillAccursed/JWE3-Crossover-Fix/issues)

Include these details in the issue.

- What happened, when it happened (with time zone), and the steps to reproduce it. Include any error message.
- JWE3 version and Steam build ID, if available.
- Mac chip, macOS version, exact CrossOver build, and the bottle’s graphics backend and D3DMetal version.
- MacGameFix version, whether **Copy protection under Rosetta** was enabled, and whether you set a `d3d12` override for `JWE3.exe`.

## Uninstall

Quit the game and Steam. Remove the shim or restore the DLL you backed up, then remove the per-game override if you added one. Turn off the Rosetta improvement in MacGameFix to restore CrossOver's original Wine module. To restore the original vendor-signed app, use the saved copy or reinstall the matching CodeWeavers build.

## Tested setup

- **JWE3 1.5.6 (Steam build `26407273`)** opened in a direct launch with both fixes on an M4 Pro, macOS 27.2, CrossOver Preview `27.0.0.41073`, and D3DMetal 4 beta 3. The normal Steam **Play** launch after installing the module into Preview is pending verification.
- **Earlier Steam build `25967550`** launched through Steam with the D3D12 shim alone. The menu, settings, and first campaign level worked. Camera movement, textures, and models were checked in that level.

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

MacGameFix and Hrava provided the Rosetta compatibility work. The heap mirroring approach comes from fladnaG86's FH2-Port-AppleSilicon. [MacGameFix engine notes](https://github.com/MathiasKowoll/MacGameFix/blob/main/engine/README.md#rosettas-cpu-for-copy-protection) · [FH2-Port-AppleSilicon](https://github.com/fladnaG86/FH2-Port-AppleSilicon) · [Project license](LICENSE)
