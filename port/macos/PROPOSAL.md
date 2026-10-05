# Proposal: macOS on Apple silicon (arm64)

Status: proposal. Nothing in this folder builds the game yet. The programs in
[`measurements/`](measurements) are the evidence for the decisions here.

Target: every Apple silicon Mac, from the M1 MacBook Air (8 GB, no fan,
60 Hz) up. The game must run as native arm64 code, with no Rosetta 2.

## Summary

The Android port already compiles the game as ILP32 AArch64 code
(`arm64_32`) and runs it as a guest image below 4 GB, inside a 64-bit host
process. On Android, the guest and the host share one process.

macOS does not allow that. A native arm64 process on macOS cannot map
anything below 4 GB (measured below). The Android guest must be at
`0x80000000`.

The proposal: **run the same `arm64_32` guest image in a small virtual
machine with Apple's Hypervisor.framework**. The macOS app is the host. The
guest code runs on the CPU at native speed, at the addresses that it was
built for, with 4 KB pages. The guest calls the host through `hvc`
instructions in place of the Android branch stubs. The OpenGL calls, which
are most of those calls, go through a command ring, not one exit per call.
The graphics use the OpenGL ES 3 renderer of Android, on ANGLE's Metal
backend.

This plan uses most of the Android work again: the guest build, the musl
`arm64_32` C library, the import generator, the ABI checks, the ES 3
renderer and the PGO profile. Most of the new code is the host. The game
source does not change.

## The constraint, measured

All measurements: MacBook Air (M1, 8 GB), macOS 26.6.1, Apple clang 17.

| Test | Result | Program |
| --- | --- | --- |
| arm64 process: page size | 16384 bytes | `low_memory.c` |
| arm64 process: `mmap(0x80000000, 128 MB, MAP_FIXED)` | Fails, `ENOMEM`. The low 4 GB is the hard `__PAGEZERO`. | `low_memory.c` |
| arm64 process linked with `-pagezero_size 0x4000` | The kernel kills it at start (exit 137) | `low_memory.c` |
| x86_64 process under Rosetta 2, small `__PAGEZERO` | Page size 4096. The fixed mapping succeeds. | `low_memory.c` |
| Hypervisor.framework: one `hvc` exit and return | 710 to 860 ns (three runs, 200,000 exits each) | `hvf_exit.c` |
| Hypervisor.framework limits | 64 vCPUs, 36-bit guest physical addresses | `hvf_limits.c` |

Two problems of Android carry over to macOS, and both are worse:

1. **The low 4 GB.** Android can claim it with luck (`host_memory.c`
   moves ART out of the way). macOS never gives it to an arm64 process.
2. **16 KB pages.** The Android README says that kernels with 16 KB pages
   do not operate, because the Xbox memory uses 4 KB pages
   (`xbox_memory.c` commits and protects 4 KB pages, `memory_watch.c`
   write-protects 4 KB texture pages). All Apple silicon processes have
   16 KB pages.

## Options considered

| Option | What it is | Speed on M1 | Work | Verdict |
| --- | --- | --- | --- | --- |
| **A. Hypervisor guest** | The Android guest image in a Hypervisor.framework VM. The macOS app is the host. | Native for game code. Each host call costs ~0.8 µs, so GL calls must be batched. | Medium. A new host. The guest changes only in its stubs and a small EL1 runtime. | **Proposed** |
| B. Rosetta 2 | An x86-64 host with the 32-bit x86 game code, as Wine does on Apple silicon | Translated code, and 32-bit code segments need Wine-style tricks | Medium | Rejected. Apple said that general Rosetta 2 support ends after macOS 27. It is not "native" or "optimal". |
| C. LP64 port | Make the game correct with 64-bit pointers | Native | Very large. The cache files and saved games hold 32-bit pointers, and the game state is at fixed addresses (16 MB at `0x81A00000`). Each tag structure with a pointer changes layout. | Rejected for now. Too many changes in the decompilation, which upstream keeps close to the original. |
| D. wasm32 and wasm2c | Compile the game to WebAssembly (also ILP32), then to native code with a base register | Estimated 10–40% slower than native (bounds tricks, indirect call checks) | Large. New toolchain, threads, the declarations that disagree with their definitions trap. | Plan B, if A fails in Phase 0 |
| E. Compiler fork | Change LLVM's `arm64_32` code generation to address `[base, wN, uxtw]` | Native | Large, and an LLVM fork to keep up to date | Rejected. The cost of maintenance is too high. |

Why A is the best fit for "optimal on M1":

- The CPU work of the game runs as native AArch64 code. The second stage of
  address translation costs some TLB performance, and Phase 0 measures it.
- The guest has its own stage-1 page tables, so it can use **4 KB pages**
  again. `xbox_memory.c` and `memory_watch.c` stay as they are.
- A write to a watched texture page is a permission fault that the guest's
  own EL1 handler records. That costs no exit.
- `QueryPerformanceCounter` and the other timers read `CNTVCT_EL0` in the
  guest, with no exit.
- The renderer's GL calls move to a host render thread, so the GL driver
  works on a different core from the game. An M1 has four performance
  cores and four efficiency cores.

## Proposed design

```
macOS process (arm64, Mach-O, 16 KB pages)
┌─────────────────────────────────────────────────────────────────────────┐
│ host (new: port/macos/host)                                             │
│   loader ── reads halo_guest.elf, fills the import table                │
│   vCPU threads ── one host thread and one vCPU for each guest thread    │
│   exit handler ── hvc #n → host import n (syscalls, SDL, files, sockets)│
│   GL decoder ── render thread, reads the command ring, calls ANGLE      │
│   SDL3 (Cocoa, CoreAudio, GameController)    ANGLE (Metal backend)      │
│                                                                         │
│   guest memory: host allocations, mapped into the VM with hv_vm_map     │
│   ┌───────────────────────────────────────────────────────────────────┐ │
│   │ VM (guest addresses = guest physical addresses, 4 KB stage-1)     │ │
│   │  0x80000000  Xbox contiguous window, 128 MB                       │ │
│   │  0x88000000  guest image: game + port/linux/src + musl arm64_32   │ │
│   │              + EL1 runtime (vectors, page tables, fault handler)  │ │
│   │  pools       malloc arenas, thread stacks, the GL command ring    │ │
│   └───────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

### The guest

It is the Android guest image (`build/android/halo_guest.elf`), with three
changes:

1. **Import stubs.** `tools/android_imports.py` writes, for each import,
   `adrp/ldr/br x16` (a branch to a 64-bit host address). For macOS it
   writes `mov w16, #<index>` and `hvc #0`. The arguments stay in their
   registers, as now. The host reads x0–x7 and d0–d7 from the vCPU, calls
   the function, and writes x0/d0 back. The two ABIs already agree on these
   registers (`halo_android_abi.h`).
2. **An EL1 runtime** (new, small, `port/macos/guest/el1.S` and `el1.c`).
   The guest runs at EL1 with its own exception vectors:
   - Identity page tables (guest virtual address = guest physical address),
     4 KB granule, normal cacheable memory. With the MMU off, all memory is
     Device memory, which is slow and faults on unaligned access.
   - A permission fault in a watched page records the write and makes the
     page writable, as `host/host_memory.c` does with `SIGSEGV` now. Other
     faults exit to the host, which prints the registers and the frame
     chain, as Android does.
   - `mprotect`/`mmap` of guest memory changes these page tables. Most such
     calls need no exit.
3. **GL stubs that encode.** Refer to "OpenGL calls".

The image keeps the Android address layout. `guest.ld`,
`halo_android_abi.h`, the musl port and `android_abi_check.py` do not
change. The guest is compiled for the same target as on Android
(`arm64_32-apple-watchos`), so the Android PGO profile applies to it.

`HALO_ANDROID` is the switch for the guest today. A second switch,
`HALO_GUEST_HVF`, selects the three changes above. The game source does not
see it.

### The host

`port/macos/host` is a port of `port/android/host`:

| Android file | macOS equivalent |
| --- | --- |
| `host_memory.c` (claims low addresses, `SIGSEGV` watch) | Allocates guest memory with `mmap` anywhere, and maps it into the VM with `hv_vm_map` at the guest address. Stage 2 uses 16 KB granules, so the window and the pools are 16 KB aligned. They are now. The host reads guest memory at `host_base + guest_address`. |
| `host_loader.c` (ELF loader) | The same code, with the host's own symbol table. |
| `host_thread.c` (guest threads on guest stacks) | Each guest thread is a host `pthread` with its own vCPU. `hv_vcpu_run` loops in that thread. The vCPU of a thread must be made on that thread. |
| `host_syscall.c` (Linux system calls of musl) | Darwin system calls for the same numbers: files, `futex` (as `__ulock_wait`/`__ulock_wake` or a condition variable), `clock_gettime`, `nanosleep`, sockets. |
| `host_sdl.c` | The same SDL3 calls. The audio callback runs the guest's mixer on the audio thread's own vCPU, as the Android host gives it a guest stack. |
| `host_gl.c` | The decoder of the GL command ring, and the synchronous GL calls. |
| `host_main.c` (SDL activity) | `main()` of the app. Paths: `~/Library/Application Support/OpenCE`. |

The Android host is about 2,400 lines of C. Most of it ports directly.

### OpenGL calls

On Android, a GL call is a branch to the host. With Hypervisor.framework, a
host call is an exit of ~0.8 µs. How many GL calls does a frame make? The
renderer (`d3d8_gl.c`) keeps the GL state and calls GL only when the state
changes. A draw then makes a few calls: uniforms, a buffer upload, the
vertex format, the draw. The Linux build counts the draws
(`stats.draws / stats.presents`, `d3d8_gl.c:3735`), but this proposal has
no numbers from it. For a scale estimate only:

| Draws per frame (assumed) | GL calls per draw (assumed) | Exits per frame | Cost at 0.8 µs | Part of a 60 Hz frame | Part of a 120 Hz frame |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 5 | 5,000 | 4 ms | 24% | 48% |
| 2,000 | 6 | 12,000 | 9.6 ms | 58% | 115% |

One exit for each GL call is thus acceptable for the first boot, but not
for a release. The design:

- `port/linux/src/gl.h` lists 109 GL functions. `tools/android_gl_stubs.py`
  makes their stubs. A new mode of that tool makes, for each function with
  no result, a stub that writes the function's number and its arguments
  into a ring in guest memory. A pointer argument copies its data into the
  ring (the size comes from the arguments: `glBufferSubData` size,
  `glUniform4fv` count × 16, and so on), because GL copies at the call, and
  the game can change the memory after the call.
- About 23 functions return a value or read back (`glGet*`, `glGen*`,
  `glCreate*`, `glMapBufferRange`, `glReadPixels`, `glClientWaitSync`,
  ...). These flush the ring and exit. The guest allocates the names of
  `glGen*` itself, as gfxstream and virgl do, so these do not flush.
- The host's render thread owns the GL context and decodes the ring. The
  guest exits only when the ring is full, at a synchronous call, and at
  `SwapWindow`. A frame then has tens of exits, not thousands.
- The Android host already has the frame's points of synchronization:
  `host_gl_fence_frame` and `host_gl_wait_frame` fence each slot of the
  three-frame buffer ring, and `host_gl_buffer_write` writes into a slot
  without a wait.

Thus the GL driver's CPU work runs on another core, at the same time as the
game's next frame. On a 4+4-core M1, this is a gain, not a cost.

### Graphics

macOS stops at OpenGL 4.1 (deprecated, on Metal underneath). The Linux
renderer needs OpenGL 4.5. The Android renderer needs only OpenGL ES 3.0,
and uses ES 3.1 and 3.2 functions when they exist.

Use the ES 3 path on **ANGLE with its Metal backend**, which is how Chrome
draws WebGL on the Mac. SDL3 makes an ES context through ANGLE's `libEGL`
and `libGLESv2`, which ship inside the app.

- Apple silicon GPUs decode BC1–BC3 (S3TC). If ANGLE gives
  `GL_EXT_texture_compression_s3tc`, the DXT textures go to the GPU as they
  are, with no CPU decode (the CPU decode is for Mali).
- Atomic counters are ES 3.1. If ANGLE's Metal backend does not give them,
  the lens flares use the ES 3.0 path, as on some Android devices.
- Retina: SDL3 reports the size in pixels. The game draws 480 lines of the
  game at the display's resolution (`display.resolution_scaling =
  "native"`), as on Linux.
- Frame rate: interpolation already draws one frame for each refresh. On
  the 120 Hz ProMotion displays of the MacBook Pro, the game shows
  120 frames each second.
- Widescreen: the `HALO_ANDROID` changes in `rasterizer_xbox.c`,
  `render.c` and the others are for any display shape. Use them on macOS too.

A native Metal renderer is Phase 4 and optional. Do it only if a profile
shows that ANGLE's translation costs a significant part of the frame.

### Audio, input, network, files

- Audio: SDL3 on CoreAudio. `dsound_sdl.c` does not change.
- Input: SDL3 on GameController.framework (DualSense, Xbox and MFi
  controllers, with rumble), keyboard and mouse as on Linux.
- Network: the BSD sockets of `posix_net.c` operate on macOS with small
  changes: `MSG_NOSIGNAL` becomes `SO_NOSIGPIPE` (5 places), `getrandom`
  becomes `getentropy`, `/proc/self/exe` becomes `_NSGetExecutablePath`,
  `xdg-open` becomes `open`, and the Discord IPC socket is at
  `$TMPDIR/discord-ipc-0`. The macOS firewall asks once to accept
  connections for system link.
- Files: `maps/`, `save/`, `config.toml` and `debug.txt` go into
  `~/Library/Application Support/OpenCE`. The first start asks for the disc
  image with the system's file dialog, as on Linux.

### Packaging, signing and updates

- The app is `Halo.app`: the host executable, the guest image as a
  resource, SDL3, and ANGLE's two dylibs.
- Hypervisor.framework needs the `com.apple.security.hypervisor`
  entitlement. An ad hoc signature with this entitlement operates (the
  measurements used one). It needs no provisioning profile.
- Without a Developer ID certificate and notarization, Gatekeeper stops a
  downloaded app. The user must open it from System Settings > Privacy &
  Security one time. A Developer ID costs 99 USD each year. The project
  must decide this (refer to "Open questions").
- The updater (`posix_update.c`) replaces the whole `.app` in place and
  then starts the new one. It must keep the signature intact, and it must
  remove the quarantine attribute only from the files that it downloaded
  itself.

### Power and heat on a fanless MacBook Air

- `display.vsync = true` and the display's refresh rate are the defaults,
  as now. Without vsync, the default limit is twice the refresh rate
  (`display.max_fps = 0`). On a Mac, make the default limit equal to the
  refresh rate. Frames that are never shown only make heat.
- The game thread runs at the QoS class `user-interactive`, the GL render
  thread at `user-interactive`, and the audio thread with SDL's real-time
  priority. The loader and the disc extraction run at `utility`, on the
  efficiency cores.
- Disable App Nap while the game window is in front.
- Measure with `powermetrics` in Phase 2: the package power during the
  first level of the campaign, at 60 Hz.

### Build and CI

- `configure.py` gets a `macos` target. `tools/macos_build.py` follows
  `tools/android_build.py`: the guest build is the same, the host build
  uses the Apple clang of the system, and the bundle step signs the app.
  `ninja macos` builds `build/macos/Halo.app`. On a Mac, plain `ninja` makes
  this build.
- Requirements: Xcode command line tools, Python, ninja, and LLVM from
  Homebrew for `ld.lld` and `llvm-mc` (the guest's ELF link). For the PGO
  profile, the guest needs clang 22 or later, as on Linux. The Apple clang
  is too old for the profile.
- Mach-O assembly from clang still goes through `android_asm_convert.py` to
  ELF. A later change could link the guest as a static Mach-O image, but the
  ELF loader exists and operates.
- GitHub Actions has arm64 macOS runners, so CI can build and sign the app.
  But the runners are themselves VMs, and Apple gives nested
  virtualization only on M3 or later, with macOS 15 or later.
  Hypervisor.framework is probably not available in those VMs (Phase 0
  checks it). Until it is, CI only builds and signs the Mac app, and a
  person must start the game on a real Mac.

## Phases

Each phase ends with a result that someone can measure. The estimates are
for one developer who knows the Android port. They are estimates, not
commitments.

| Phase | Work | Result that ends the phase | Estimate |
| --- | --- | --- | --- |
| 0. Spike | Build `halo_guest.elf` on macOS. A minimal host that maps it, sets up the EL1 page tables, and runs `__guest_start` to its first host call. Measure: a CPU benchmark in the guest against the same code as a native arm64 build (the stage-2 cost); the 4 KB granule in the guest; draws and GL calls per frame on Linux, with `stats`. | The numbers in a table in this file. If the guest is more than 10% slower than native, stop, and evaluate option D. | 1–2 weeks |
| 1. Boot | The full host: memory, threads, system calls, SDL, sockets. GL with one exit for each call (correct, slow). ANGLE. | The main menu, then a campaign level, on an M1 Air. The debug build has no failed assertions. | 4–6 weeks |
| 2. Speed | The GL command ring and the render thread. Profile with Instruments. | 60 frames each second in "The Pillar of Autumn" and "Assault on the Control Room" on an M1 Air at its native resolution, with frame times in a table. Package power measured. | 3–4 weeks |
| 3. Release | The `.app` bundle, signing, the updater, the macOS paths, controllers, system link with Linux, Windows and Android machines, the README for macOS. | A release zip from CI. A system link game with all four platforms. | 2–3 weeks |
| 4. Optional | A native Metal renderer. Bink video. | Only if Phase 2's profile shows that it is necessary. | 6–10 weeks |

## Risks

| Risk | Effect | What to do |
| --- | --- | --- |
| The second stage of address translation slows the game | The game code is slower than native | Phase 0 measures it. 16 KB stage-2 pages and contiguous allocations keep TLB misses low. |
| Many synchronous GL calls in the hot path (for example `host_gl_read_buffer_word`, which reads a lens flare's sample count) | The ring flushes too often | Count the flushes in Phase 2. Read the counters one frame late, as the fences of `host_gl_fence_frame` already let the renderer do. (`glGetError` is not a problem: `gl_check_errors` calls it only with `debug.gl_debug = true`.) |
| Calls into the guest (the audio callback, SDL events) | Latency | Each is one entry into the VM, ~1 µs, about 100 times each second for audio. Negligible. |
| ANGLE's Metal backend lacks a function or has a defect | Rendering errors | The ES 3.0 path already works without the ES 3.1/3.2 functions. Apple's GL 4.1 is a second path, but it is deprecated. |
| Apple restricts Hypervisor.framework | No port | It is a public, documented framework. QEMU (and thus UTM) uses it directly, and Apple's Virtualization.framework is built on it. The risk is low. |
| Debugging in the guest | lldb cannot see guest code | The host prints the registers and the frame chain, as Android does (`llvm-symbolizer`). Later: a GDB stub on the vCPU's debug exceptions. |
| No nested virtualization on CI runners | No automatic tests on macOS | Build in CI. Test by hand on a Mac. Use M3 runners when they exist. |

## What changes outside `port/macos`

Few changes, so upstream can review them:

- `tools/android_imports.py`, `tools/android_gl_stubs.py`: a mode for
  `hvc` stubs and for encoding stubs.
- `tools/macos_build.py` (new), and `configure.py` for the target.
- `port/linux/src/posix_*.c`, `sdl_platform.c`: `#ifdef __APPLE__` for the
  Darwin differences above. These files are host code on Android, so they
  compile for the macOS host.
- The `HALO_ANDROID` widescreen changes: also for `HALO_GUEST_HVF`, or a new
  name for both, for example `HALO_WIDESCREEN`.
- No change in `source/`.

## Open questions

These are for the project, not for this proposal:

1. Does the project pay for a Developer ID and notarization, so that
   Gatekeeper does not stop the app? Without it, each new user must allow
   the app by hand.
2. What is the oldest macOS to support? Hypervisor.framework on Apple
   silicon exists since macOS 11, the first macOS for the M1. Phase 0
   confirms which version the host's calls need.
3. Is `HALO_ANDROID` the right name for the guest's switches once two
   platforms use the guest? A rename to `HALO_GUEST` touches many files.
4. Somebody with the game data on Linux: what are `stats.draws` and the GL
   calls per frame in a heavy scene? That number decides if the command
   ring is needed in Phase 1 or Phase 2.

## Measurements

To make the measurements again on a Mac:

```bash
cd port/macos/measurements
clang -O2 low_memory.c -o low_memory && ./low_memory
clang -O2 hvf_exit.c -o hvf_exit -framework Hypervisor
codesign -s - --entitlements hypervisor.entitlements -f hvf_exit && ./hvf_exit
clang hvf_limits.c -o hvf_limits -framework Hypervisor
codesign -s - --entitlements hypervisor.entitlements -f hvf_limits && ./hvf_limits
```

Each file gives its result on an M1 MacBook Air in its first comment. Add
results from other Macs (M2, M3, M4) to the table above.
