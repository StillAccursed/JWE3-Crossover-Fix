# Technical details

## Cause of the launch crash

1. During texture setup, the render thread calls `ID3D12Device3::OpenExistingHeapFromAddress`. The API wraps an existing Windows virtual memory allocation in a CPU-accessible D3D12 heap so that resources can be placed on it. [Microsoft API reference](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device3-openexistingheapfromaddress)
2. The tested D3DMetal 4 beta 3 backend returns `E_NOTIMPL` (`0x80004001`). The game treats that HRESULT as fatal. The failed call is at `JWE3.exe+0x1CD2C7D` in Steam build `25967550`. The caller’s error path and the value saved on the crash stack identify the API and its return code. The [standalone baseline](../tests/results/baseline.txt) reproduces the same HRESULT independently.
3. While gathering graphics diagnostics for that failure, the game dereferences a null pointer and faults reading address `0x10` at `JWE3.exe+0x17463CF`. This second fault explains why the original error was hidden behind an empty crash reporter.

## How the shim fixes it

1. The game loads its local `d3d12.dll`. The shim forwards the exports it provides to the bottle’s system D3D12 module, which routes rendering to D3DMetal. It installs device hooks after successful `D3D12CreateDevice` calls in `JWE3.exe` and the standalone probe. [Proxy source](../src/proxy.cpp)
2. The heap hook calls the backend first. On `E_NOTIMPL` for a supported `ID3D12Heap` request, it checks the committed memory at the allocation base and creates a CPU-visible, write-back heap. For buffers placed on that heap, the shim maps the real D3D12 resource and tracks it alongside the corresponding bytes in the original game allocation.
3. Before a hooked queue executes command lists, the shim copies CPU changes into the mapped resource. It compares the original memory and mapped resource with the last synchronized data in 64-byte blocks. On copyback it preserves bytes the CPU has edited since the previous synchronization.
4. When a hooked queue signals the game’s fence, the shim first queues an internal GPU fence. A worker waits for that fence and copies GPU changes back to game memory. The queue waits on a second internal fence before it signals the game’s fence. The worker releases that wait only after copyback, so observing the game’s fence means the returned bytes are visible.

## Verification and scope

- The standalone probe failed at `OpenExistingHeapFromAddress` with the shim in passthrough mode. With mirroring enabled, it verified CPU writes through GPU readback and verified GPU writes in game memory when the public fence completed. [Baseline output](../tests/results/baseline.txt) · [Patched output](../tests/results/probe-report.txt)
- The existing Steam installation loaded the local DLL on its normal launch. Live testing reached the first campaign level and confirmed responsive camera movement and visually correct textures and models on the tested setup. Longer sessions, later levels, and other CrossOver or game builds are unverified.
- The fallback covers committed memory ranges starting at the allocation base and standard placed buffers. It limits mirrored buffer data to 256 MiB in total. If GPU-to-CPU synchronization fails, the queue wait stays pending because the shim cannot safely report that the data is ready.

For diagnostics, launch through CrossOver with `WINEDLLOVERRIDES=d3d12=n,b` if necessary. Set `JWE3_D3D12_LOG` to a writable Windows file path for shim logs or `JWE3_PROBE_REPORT` for probe output. Set `JWE3_D3D12_PASSTHROUGH=1` to disable the device hooks for a baseline run.

## Credits

fladnaG86’s FH2-Port-AppleSilicon demonstrated the same missing D3DMetal API and the mapped-buffer mirroring and fence-ordered copyback approach. This project adapts that design for JWE3, with a queue wait to order copyback before the game’s fence. The upstream MIT copyright notice is preserved. [Reference implementation at the revision consulted](https://github.com/fladnaG86/FH2-Port-AppleSilicon/blob/cb0164e640e3362aac4fbdd75a00d437c5c00ddf/src/d3d12/d3d12.c) · [Upstream MIT notice](../LICENSE.fh2)
