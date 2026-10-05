# Proposal: macOS on Apple silicon (arm64)

Status: proposal. Nothing in this folder builds the game yet. The programs in
[`measurements/`](measurements) are the evidence for the decisions here.
Numbers without a program are estimates, and the text says so.

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
machine with Apple's Hypervisor.framework**. The macOS app is the host.

- The guest code runs on the CPU at native speed, at the addresses that it
  was built for (measured: within 1% on CPU-bound code).
- The guest has its own page tables, so the 4 KB pages of the Xbox memory
  operate again (measured).
- The texture watch becomes a fault in the guest, handled in the guest,
  about ten times faster than the `SIGSEGV` path of Android (measured).
- The guest calls the host through `hvc` instructions in place of the
  Android branch stubs. Each such call costs about 0.75 µs (measured), so
  the frequent calls stay in the guest, and the OpenGL calls go through a
  command ring.
- The graphics use the OpenGL ES 3 renderer of Android, on ANGLE's Metal
  backend (measured: ES 3.0, with S3TC).

This plan uses most of the Android work again: the guest build, the musl
`arm64_32` C library, the import generators, the ABI checks and the ES 3
renderer. Most of the new code is the host and a small EL1 runtime in the
guest. The game source in `source/` does not change.

## Measurements

All measurements: MacBook Air (M1, 8 GB), macOS 26.6.1, Apple clang 17.
Each program gives its build commands in its first comment.

### The constraint

| Test | Result | Program |
| --- | --- | --- |
| arm64 process: page size | 16384 bytes | `low_memory.c` |
| arm64 process: `mmap(0x80000000, 128 MB, MAP_FIXED)` | Fails, `ENOMEM`. The low 4 GB is the hard `__PAGEZERO`. | `low_memory.c` |
| arm64 process linked with `-pagezero_size 0x4000` | The kernel kills it at start (exit 137) | `low_memory.c` |
| x86_64 process under Rosetta 2, small `__PAGEZERO` | Page size 4096. The fixed mapping succeeds. | `low_memory.c` |

Two problems of Android carry over to macOS, and both are worse:

1. **The low 4 GB.** Android can claim it with luck (`host_memory.c`
   moves ART out of the way). macOS never gives it to an arm64 process.
2. **16 KB pages.** The Android README says that kernels with 16 KB pages
   do not operate, because the Xbox memory uses 4 KB pages
   (`xbox_memory.c:24`, `memory_watch.c:30`). All Apple silicon processes
   have 16 KB pages.

### Hypervisor.framework

| Test | Result | Program |
| --- | --- | --- |
| One `hvc` exit to the host and return | 710 to 860 ns (six runs, 200,000 exits each) | `hvf_exit.c` |
| Limits | 64 vCPUs, 36-bit guest physical addresses | `hvf_limits.c` |
| Stage-1 granules that the vCPU reports (`ID_AA64MMFR0_EL1`) | 4 KB and 16 KB: yes. 64 KB: no. | `hvf_mmu.c` |
| 4 KB guest pages with the MMU on | Operate. 16,384 writes to 16,384 read-only 4 KB pages make 16,384 faults. | `hvf_mmu.c` |
| CPU-bound code (a dependent multiply chain), guest against native | Within ±0.6% (three runs) | `hvf_mmu.c` |
| A write fault on a watched page, handled by an EL1 handler in the guest (no exit) | 239 to 470 ns | `hvf_mmu.c` |
| The same, natively, as Android does it: `SIGBUS` handler and `mprotect` of a 16 KB page | 4,285 to 4,503 ns | `hvf_mmu.c` |
| An ad hoc signature with the `com.apple.security.hypervisor` entitlement | Operates | all `hvf_*.c` |

The cost of the second stage of address translation. A chain of dependent
32-bit loads at random 64-byte lines, the same instructions natively and in
the guest. Nanoseconds for each load, median of three runs:

| Working set | Native (16 KB pages) | Guest, 4 KB pages | Guest, 2 MB blocks |
| --- | --- | --- | --- |
| 1 MB | 5.6 | 6.3 (+13%) | 5.6 (+0%) |
| 4 MB | 6.4 | 7.4 (+16%) | 6.5 (+2%) |
| 16 MB | 48.9 (noisy: 44.8 to 54.3) | 54.4 (+11%) | 45.2 |
| 64 MB | 92.0 | 113.8 (+24%) | 98.2 (+7%) |
| 256 MB | 105.0 | 135.9 (+29%) | 119.3 (+14%) |

This is the worst case: each load misses the TLB on purpose. Game code has
more locality, so its cost is smaller (an estimate; Phase 0 measures the
game). The table gives one rule for the design: **map the guest memory with
2 MB blocks, and split a block into 4 KB pages only where the game protects
or watches pages.**

### Graphics: ANGLE on Metal

`angle_caps.c` loads the ANGLE of an installed Electron 42.10.0 (the copy in
Visual Studio Code) with the Metal backend, and makes an ES 3 context:

| Item | Result |
| --- | --- |
| `GL_VERSION` | OpenGL ES 3.0 (ANGLE 2.1). No ES 3.1. |
| `GL_RENDERER` | ANGLE Metal Renderer: Apple M1 |
| `GL_MAX_FRAGMENT_ATOMIC_COUNTERS` | 0 |
| `GL_EXT_texture_compression_dxt1`, `GL_ANGLE_texture_compression_dxt3`, `GL_ANGLE_texture_compression_dxt5` | Yes. `gl_initialize` accepts these three for S3TC (`d3d8_gl.c:910`). |
| `GL_EXT_texture_compression_s3tc` | No |
| `GL_EXT_texture_filter_anisotropic`, `GL_EXT_clip_control`, `GL_EXT_occlusion_query_boolean` | Yes |
| `GL_EXT_draw_elements_base_vertex`, `GL_OES_draw_elements_base_vertex` | Yes |
| `GL_EXT_copy_image`, `GL_OES_copy_image`, `GL_EXT_texture_border_clamp`, `GL_OES_texture_border_clamp`, `GL_EXT_buffer_storage` | No |
| CPU time to submit one draw with one uniform change | 127 to 147 ns (five frames of 20,000 draws) |

The renderer has a path for each missing item. These paths already operate
on Android devices with ES 3.0:

- No `copy_image`: a blit (`copy_level_by_blit`, `d3d8_gl.c:2167`).
- No `border_clamp`: clamp to the edge (`d3d8_gl.c:2007`).
- No atomic counters: the lens flares use an occlusion query, which tells
  only whether a sample is visible (`d3d8_gl.c:1480-1495`).

One change helps: `gl_initialize` sets `base_vertex` only for ES 3.2
(`d3d8_gl.c:901`). ANGLE gives the extension on ES 3.0. If the renderer
accepts the extension, the CPU does not rewrite the indices of each indexed
draw.

### Not measured yet

- **Draws and GL calls per frame.** The game data was not available. The
  renderer counts draws (`stats` at `d3d8_gl.c:385-393`, printed every 60
  frames with `debug.gpu_stats`). The Android build can print them too.
- **The game in the guest.** The microbenchmarks above bound the cost.
  Only the game itself gives the real number (Phase 0).
- **A build of the guest on macOS.** Nothing is built yet (refer to "Build
  and CI").

## Options considered

| Option | What it is | Speed on M1 | Work | Verdict |
| --- | --- | --- | --- | --- |
| **A. Hypervisor guest** | The Android guest image in a Hypervisor.framework VM. The macOS app is the host. | Native for CPU work (measured). Memory: 0–14% slower on random access with 2 MB blocks (measured worst case). Each host call costs ~0.75 µs, so frequent calls must stay in the guest. | Medium. A new host, a small EL1 runtime, changes to the stub generators. | **Proposed** |
| B. Rosetta 2 | An x86-64 host with the 32-bit x86 game code, as Wine does on Apple silicon | Translated code. 32-bit code segments need Wine-style tricks. | Medium | Rejected. Not native. Apple: Rosetta is "available through macOS 27" as a general tool; after that, only "a subset … aimed at supporting older unmaintained gaming titles". |
| C. LP64 port | Make the game correct with 64-bit pointers | Native | Very large. The cache files and saved games hold 32-bit pointers, and the game state is at a fixed address (16 MB at `0x81A00000`, `halo_port_capacity.h:31-34`). Each tag structure with a pointer changes layout. | Rejected for now. Too many changes in the decompilation, which upstream keeps close to the original. |
| D. wasm32 and wasm2c | Compile the game to WebAssembly (also ILP32), then to native code with a base register | Estimated 10–40% slower than native (bounds tricks, indirect call checks). Not measured. | Large. New toolchain, threads. Calls whose declaration and definition disagree trap. | Plan B, if A fails in Phase 0 |
| E. Compiler fork | Change LLVM's `arm64_32` code generation to address `[base, wN, uxtw]` | Native | Large, and an LLVM fork to keep up to date | Rejected. The cost of maintenance is too high. |

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
│   guest memory: one host allocation, mapped into the VM with hv_vm_map  │
│   ┌───────────────────────────────────────────────────────────────────┐ │
│   │ VM (guest virtual = guest physical; 2 MB blocks, 4 KB where       │ │
│   │ watched)                                                          │ │
│   │  0x80000000  Xbox contiguous window, 128 MB                       │ │
│   │  0x88000000  guest image: game + port/linux/src + musl arm64_32   │ │
│   │              + EL1 runtime (vectors, page tables, fault handler)  │ │
│   │  pools       malloc arenas, thread stacks, the GL command ring    │ │
│   └───────────────────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
```

The host reserves the whole guest space as one allocation and maps it with
one `hv_vm_map`. A guest address is then `host_base + address` in the host.

### The guest

It is the Android guest image (`build/android/halo_guest.elf`). The address
layout, `guest.ld` and `halo_android_abi.h` do not change. The compiler
target does not change (`arm64_32-apple-watchos`). The changes:

1. **Import stubs.** `tools/android_imports.py` writes, for each import,
   `adrp/ldr/br x16` to a 64-bit host address (`android_imports.py:35-44`).
   For macOS it writes `hvc #<index>` and `ret`. The index comes back to the
   host in the exit syndrome. There are about 194 imports: 51 host, 45
   posix, 98 GL. The host reads x0–x7 and d0–d7. Seven imports also have
   arguments on the stack: `glTexImage2D`, `glTexImage3D`,
   `glTexSubImage2D`, `glCompressedTexImage3D`, `glBlitFramebuffer`,
   `glCopyImageSubData` and `posix_socket_select`. The host reads these at
   `host_base + sp`. No import is variadic, returns a structure or returns
   a float.
2. **An EL1 runtime** (new, small, `port/macos/guest/el1.S` and `el1.c`).
   The guest runs at EL1 with its own exception vectors. `hvf_mmu.c` is a
   working model of it.
   - Identity page tables (guest virtual address = guest physical address).
     Normal, write-back, inner shareable memory. The guest's atomics are
     load and store exclusive (`-mcpu=cortex-a53`). They need Normal,
     shareable memory across the vCPUs.
   - 2 MB blocks by default. A block becomes 4 KB pages when the game
     protects or watches a page in it.
   - `CPACR_EL1` enables the FP unit. The guest writes `FPCR` itself
     (`msvc_crt.c:311-320`).
   - `mmap`, `munmap` and `mprotect` of guest memory change these page
     tables in the guest, with no exit.
   - Other faults exit to the host, which prints the registers and the
     frame chain, as Android does.
3. **The watch moves into the guest.** On Android the guest does not
   compile `memory_watch.c` (`android_build.py:430`).
   `guest_memory_watch.c` forwards each call to the host. The host uses
   `mprotect` on 4 KB pages and a `SIGSEGV` handler (`host_memory.c:473-600`).
   The watch also covers the mirrored vertex and index pages
   (`d3d8_gl.c:2946`), not only the textures (`xbox_textures.c:813`). On
   macOS, the watch is guest code: the EL1 fault handler records the write
   and makes the page writable. `memory_watch_generation`, which the
   renderer calls for each page in each frame, becomes a read of guest
   memory, not a host call.
4. **The thread pointer.** `pthread_arch.h` calls the `host_get_tp` import
   for each `__pthread_self()` (`guest_thread.c:52`), thus for each `errno`
   access and each emulated TLS access. As an exit, this would cost
   ~0.75 µs each time. Each vCPU has its own `TPIDR_EL0`, so the guest
   keeps the thread pointer there and reads it with one `mrs`.
5. **The clock.** `QueryPerformanceCounter` and `GetTickCount` call
   `clock_gettime(CLOCK_MONOTONIC)` (`xbox_kernel.c:644-664`), which is a
   host system call (the `arm64_32` musl has no vDSO). The guest's
   `clock_gettime` reads `CNTVCT_EL0` and `CNTFRQ_EL0` for the monotonic
   clock instead, with no exit.
6. **GL stubs that encode.** Refer to "OpenGL calls".

The switch for the guest today is `HALO_ANDROID`. It selects three things
at once: the guest ABI, the ES renderer and some Android behavior. For
example, the frame limiter is `#ifndef HALO_ANDROID` (`sdl_platform.c:799-826`),
and the guest ignores `display.resolution_scaling` (`d3d8_gl.c:88-118`).
The macOS guest needs the first two and not the third. Split the switch:
`HALO_GUEST` (the ABI), `HALO_GLES` (the renderer), and `HALO_ANDROID` for
the rest. The game source does not see these switches except at the 18
lines that already use `HALO_ANDROID` in `source/` (section names, a float
argument in `hs.c`, the stack walker).

### The host

`port/macos/host` is a port of `port/android/host` (2,464 lines):

| Android file | Lines | macOS equivalent |
| --- | --- | --- |
| `host_memory.c` (claims low addresses, `SIGSEGV` watch) | 684 | Smaller. One allocation for the guest space, one `hv_vm_map`. The watch is in the guest. |
| `host_loader.c` (ELF loader) | 116 | The same code. It fills the import table with indices. |
| `host_thread.c` (guest threads on guest stacks) | 202 | Each guest thread is a host `pthread` with its own vCPU. `hv_vcpu_run` loops on that thread. A vCPU must be made on its own thread. |
| `host_syscall.c` (Linux system calls of musl) | 376 | Much more work. Refer to "System calls". |
| `host_sdl.c` | 407 | The same SDL3 calls, with guest pointers changed to host pointers. The audio callback enters the guest on the audio thread's vCPU. |
| `host_gl.c` | 136 | The decoder of the GL command ring, the render thread, and the synchronous GL calls. |
| `host_main.c` (SDL activity) | 327 | `main()` of the app. Paths: `~/Library/Application Support/OpenCE`. |
| `host_debug.c`, `host.h` | 216 | The registers and the frame chain come from the vCPU. |

The host also links `posix_files.c`, `posix_net.c` and `posix_upnp.c` from
`port/linux/src`, as on Android.

**Guest pointers are not host pointers.** On Android, a guest address is a
valid host address. Under Hypervisor.framework it is not. Each pointer
argument must become `host_base + address`. The stubs are generated from
the prototypes (`android_gl_stubs.py`, `android_posix_stubs.py`), so the
host side of each stub can be generated with the change. Some cases need
code by hand:

- Pointers inside structures: `iovec`, `msghdr`, the `glShaderSource`
  string array (its stub already widens the array, `android_gl_stubs.py:84-104`).
- GL "pointers" that are buffer offsets and must not change:
  `glVertexAttribPointer` with a bound buffer (`d3d8_gl.c:579-581`) and the
  index offsets of `glDrawElements`.

**One exit for each posix call.** Each posix stub calls the host, then
calls `host_errno` (`android_posix_stubs.py:46-52`). On macOS the host
returns `errno` in `x1` of the same exit.

**Calls into the guest.** `host_call_guest` calls guest function pointers
directly (`host_thread.c:65-74`). The audio callback is one
(`host_sdl.c:291`). On macOS each is an entry into the vCPU, with a return
address that traps to the host.

**Host writes into watched memory.** When the host writes guest memory (for
example `read` into a texture), it does not go through the guest's page
tables, so the guest's watch does not see it. On Android such a write fails
with `EFAULT`. The host must record these writes in the watch, or a guest
wrapper must touch the pages first.

**Threads.** The game runs guest code on these threads: main, vertical
blank (`d3d8_gl.c:673`), audio (`host_sdl.c:276-360`), cache I/O
(`cache_files_windows.c:1132`), decompression
(`cache_files_decompress_windows.c:1799`), input (`input_xbox.c:1198`),
bungie_net (`thread_win32.c:117`), p2p and UPnP (`p2p.c:3046`, `2631`), the
first-run extraction (`sdl_platform.c:182`) and a silent audio clock
(`dsound_sdl.c:501`). Thus 7 to 10 vCPUs. The limit is 64.

### System calls

The guest's musl sends each system call to one import, `host_syscall`
(7 × 64-bit arguments). `host_syscall.c:184-376` has 91 cases:

- 25 are translated (write, readv, clock_gettime, nanosleep, futex, ppoll,
  mmap, mprotect and others, and stubs for brk, sigaction and others).
- 21 return `-ENOSYS`.
- About 45 go to the Linux kernel unchanged with `syscall(number, ...)`:
  `openat`, `read`, `newfstatat`, `getdents64`, `fcntl`, `getrandom` and
  others.

macOS has no Linux system calls. Each passthrough becomes a translation:
`open` flags, `AT_FDCWD`, `struct stat`, `getdents64` (from `readdir`),
`fcntl` commands, and the `errno` numbers above 34. `futex` becomes
`__ulock_wait` and `__ulock_wake`, or a condition variable. `mmap`,
`munmap` and `mprotect` stay in the guest (EL1 runtime). This file is the
largest piece of new host code. Estimate: 600 to 900 lines.

### OpenGL calls

On Android, a GL call is a branch to the host. With Hypervisor.framework, a
host call is an exit of ~0.75 µs. The renderer (`d3d8_gl.c`) keeps the GL
state in `gl_state` (`d3d8_gl.c:427-485`) and calls GL only when the state
changes. But each draw still makes some calls: `glUniform4fv`
(`d3d8_gl.c:2635`), `host_gl_buffer_write` for each streamed range
(`d3d8_gl.c:3107`, `3170`), and the draw. The number of draws in a frame is
not measured. For scale only, with assumed numbers:

| Draws per frame (assumed) | GL calls per draw (assumed) | Exits per frame | Cost at 0.75 µs | Part of a 60 Hz frame | Part of a 120 Hz frame |
| --- | --- | --- | --- | --- | --- |
| 1,000 | 5 | 5,000 | 3.8 ms | 23% | 45% |
| 2,000 | 6 | 12,000 | 9.0 ms | 54% | 108% |

One exit for each GL call is thus acceptable for the first boot, but not
for a release. The design:

- The Android list in `port/linux/src/gl.h:31-131` has 99 GL functions.
  `tools/android_gl_stubs.py` makes their stubs. A new mode makes, for each
  function with no result, a stub that writes the function's number and
  its arguments into a ring in guest memory. A pointer argument copies its
  data into the ring (the size comes from the arguments: `glBufferSubData`
  size, `glUniform4fv` count × 16, and so on), because GL copies at the
  call, and the game can change the memory after the call.
- 19 functions return a value or write through a pointer, and
  `glFinish` waits. They are handled so that few of them flush the ring:
  - The six `glGen*` functions: the guest allocates the names itself, as
    gfxstream and virgl do. No flush.
  - `glCreateShader`, `glCreateProgram`: the same.
  - `glGetString`, `glGetIntegerv`, and the imports `host_gl_get_string`
    and `host_gl_has_extension`: called at start (`gl_initialize`). The
    host answers them from a table that it fills at start. No flush.
  - `glGetQueryObjectuiv` (the lens flares on ES 3.0): the render thread
    writes each query's result into guest memory when it is available. The
    guest reads it there. The renderer already accepts a result that is
    not ready (`D3DERR_TESTINCOMPLETE`, `d3d8_gl.c:1482`). No flush.
  - `glGetShaderiv`, `glGetProgramiv`, their info logs,
    `glGetUniformLocation`, `glCheckFramebufferStatus`: at shader and
    target creation, not in each frame. They flush.
  - `glGetError`: only with `debug.gl_debug = true` (`d3d8_gl.c:2460-2474`).
  - `glReadPixels`: screenshots. It flushes.
  - `host_gl_wait_frame`: once in each frame, to wait for the fence of a
    slot of the three-frame ring (`host_gl.c:84-105`, `d3d8_gl.c:3715-3717`).
- `host_gl_read_buffer_word` reads the atomic counter of the lens flares in
  the same frame (`d3d8_gl.c:1459-1466`). It waits for the GPU. On ANGLE
  there are no atomic counters, so this path is not used on macOS.
- The host's render thread owns the GL context and decodes the ring. The
  guest exits only when the ring is full, at a synchronous call, and at
  `SwapWindow`. A frame then has tens of exits, not thousands.

Thus the GL driver's CPU work runs on another core, at the same time as the
game's next frame. ANGLE's own cost is at least 127 to 147 ns for each draw
(measured with one uniform change, the minimum). For 2,000 draws (assumed),
that is 0.3 ms or more on the render thread.

### Graphics

macOS stops at OpenGL 4.1 (deprecated, on Metal underneath). The Linux
renderer needs OpenGL 4.5. The Android renderer needs only OpenGL ES 3.0,
and uses ES 3.1 and 3.2 functions when they exist.

Use the ES 3 path on **ANGLE with its Metal backend**, which is how Chrome
draws WebGL on the Mac. ANGLE gives ES 3.0 on Metal (measured above).

- SDL3 makes an ES context through ANGLE's `libEGL` and `libGLESv2` when
  the app asks for an ES profile. The two libraries ship inside the app.
- A default ANGLE build for macOS has both its OpenGL and its Metal
  backends, and it chooses OpenGL by default. Build ANGLE with
  `angle_enable_gl=false`, or ask for the Metal platform type through
  `SDL_EGL_SetAttributeCallbacks`.
- S3TC operates (measured), so the DXT textures go to the GPU as they are.
  The CPU decode is only for Mali.
- Retina: SDL3 reports the size in pixels. The Android guest draws at scale
  1.0: 480 lines, then scales up (`d3d8_gl.c:88-118`). The desktop path
  draws 480 lines of the game at the display's resolution
  (`display.resolution_scaling = "native"`). With the switches split
  (refer to "The guest"), the macOS guest uses the desktop path.
- Widescreen: the game source calls `halo_screen_width()`, which all native
  ports share (`d3d8_gl.c:123`). Only `screen_mode_choose`
  (`d3d8_gl.c:88-118`) is Android code. The Android README says that the
  widescreen changes are in `#ifdef HALO_ANDROID` in the game source. That
  is no longer true: no such lines are in those files.
- Frame rate: interpolation already draws one frame for each refresh. On
  the 120 Hz ProMotion displays of the MacBook Pro, the game shows
  120 frames each second.

A native Metal renderer is Phase 4 and optional. Do it only if a profile
shows that ANGLE's translation costs a significant part of the frame.

### Audio, input, network, files

- Audio: SDL3 on CoreAudio. `dsound_sdl.c` does not change.
- Input: SDL3 on GameController.framework (DualSense, Xbox and MFi
  controllers, with rumble), keyboard and mouse as on Linux.
- Network: the BSD sockets of `posix_net.c` need these changes on macOS:
  - **`sockaddr`.** The game's address structures (a 2-byte family) go to
    `bind`, `connect`, `sendto`, `recvfrom` and `accept` unchanged
    (`posix_net.c:145-200`). BSD `sockaddr` has `sa_len` and a 1-byte
    family. Each of these calls must convert the address.
  - `MSG_NOSIGNAL` (3 places: `posix_net.c:185`, `191`, `815`) becomes
    `SO_NOSIGPIPE` on the socket.
  - `SOCK_CLOEXEC` and `SOCK_NONBLOCK` in `socket()` (`posix_net.c:137`,
    `483`, `783`) become `fcntl`. `accept4` (`posix_net.c:176`) becomes
    `accept` and `fcntl`.
  - `getrandom` (`posix_net.c:509`) becomes `getentropy`.
  - `/proc/self/exe` (`posix_net.c:689`, `posix_update.c:527`,
    `xbox_files.c:72`) becomes `_NSGetExecutablePath`. `/proc/self/cmdline`
    (`posix_net.c:574`) becomes the arguments of `main`.
  - The `halo://` link handler is a `.desktop` file and `xdg-mime` on Linux
    (`posix_net.c:685-733`). On macOS it is `CFBundleURLTypes` in
    `Info.plist`, and an Apple event handler for the link.
  - Discord IPC: `posix_net.c:747` already looks in `TMPDIR`, where Discord
    puts its socket on macOS (not tested).
  - The macOS firewall asks once to accept connections for system link.
- Files: `st_mtim` (`posix_files.c:33-34`) is `st_mtimespec` on macOS.
  `maps/`, `save/`, `config.toml` and `debug.txt` go into
  `~/Library/Application Support/OpenCE`. The first start asks for the disc
  image with the system's file dialog, as on Linux.

### Packaging, signing and updates

- The app is `Halo.app`: the host executable, the guest image as a
  resource, SDL3, and ANGLE's two dylibs.
- Hypervisor.framework needs the `com.apple.security.hypervisor`
  entitlement. An ad hoc signature with it operates (measured). Apple's
  list of capabilities that need a provisioning profile does not include
  it, so a Developer ID signature should not need a profile either. This
  is not tested.
- Without a Developer ID certificate and notarization, Gatekeeper stops a
  downloaded app. The user must open it from System Settings > Privacy &
  Security one time. The Apple Developer Program costs 99 USD each year.
  The project must decide this (refer to "Open questions").
- The updater (`posix_update.c`, not in the Android build) replaces the
  whole `.app` and then starts the new one. It must keep the signature
  intact, and it must remove the quarantine attribute only from the files
  that it downloaded itself.

### Power and heat on a fanless MacBook Air

- `display.vsync = true` and the display's refresh rate are the defaults.
  Without vsync, the default limit is twice the refresh rate
  (`display.max_fps = 0`, `sdl_platform.c:799-826`). On a Mac, make the
  default limit equal to the refresh rate. Frames that are never shown
  only make heat.
- The game thread and the GL render thread run at the QoS class
  `user-interactive`. The audio thread has SDL's real-time priority. The
  loader and the disc extraction run at `utility`, on the efficiency cores.
- Disable App Nap while the game runs (`NSActivityUserInitiated`).
- Game Mode: set `LSApplicationCategoryType` to a games category and
  `GCSupportsGameMode`. macOS uses Game Mode only in full screen.
- Measure with `powermetrics` in Phase 2: the package power during the
  first level of the campaign, at 60 Hz.

### Build and CI

- The Android guest build needs the Android NDK today. Without it,
  `configure.py` writes no Android build (`android_build.py:207-210`). The
  NDK gives the guest `ld.lld`, `llvm-ar`, the compiler-rt builtins and the
  ES headers. The NDK has a macOS version, so the first step is to use it
  as it is. Later, Homebrew's LLVM and the Khronos headers can replace it.
- `configure.py` gets a `macos` target. `tools/macos_build.py` follows
  `tools/android_build.py`: the guest build is the same, the host build
  uses the Apple clang of the system, and the bundle step signs the app.
  `ninja macos` builds `build/macos/Halo.app`.
- Requirements: Xcode command line tools, Python, ninja, the Android NDK,
  and ANGLE (built once, or from a prebuilt package).
- PGO: `pgo/` has the Linux and Windows profiles. The Android guest uses
  the Linux profile (`android_build.py:351`). It needs clang 22 or later
  (`linux_build.py:229`). Apple clang 17 ignores it with a warning.
- GitHub's arm64 macOS runners are M1 machines inside VMs. GitHub says
  that they do not give nested virtualization, and a request for
  Hypervisor.framework on them was closed as not planned. So CI builds and
  signs the app, and a person, or a self-hosted Mac runner, starts the
  game.

## Phases

Each phase ends with a result that someone can measure. The estimates are
for one developer who knows the Android port. They are estimates, not
commitments.

| Phase | Work | Result that ends the phase | Estimate |
| --- | --- | --- | --- |
| 0. Spike | Build `halo_guest.elf` on macOS with the NDK. A minimal host that maps it, sets up the EL1 page tables and runs `__guest_start` to its first host call. A CPU benchmark of the game code in the guest against the same code natively (for example the `.c` files of `source/math` and the tag loader on a cache file). On Linux or Android: draws and GL calls per frame in a heavy scene, with `debug.gpu_stats`. Done: the 4 KB granule, the stage-2 cost of memory access, ANGLE's capabilities (this file). | The numbers in a table in this file. If the game code in the guest is more than 10% slower than native, stop, and evaluate option D. | 1–2 weeks |
| 1. Boot | The full host: memory, threads, system calls, SDL, sockets. The guest's thread pointer, clock and watch. GL with one exit for each call (correct, slow). ANGLE. | The main menu, then a campaign level, on an M1 Air. The debug build has no failed assertions. | 5–7 weeks |
| 2. Speed | The GL command ring and the render thread. Profile with Instruments. | 60 frames each second in "The Pillar of Autumn" and "Assault on the Control Room" on an M1 Air at its native resolution, with frame times in a table. Package power measured. | 3–4 weeks |
| 3. Release | The `.app` bundle, signing, the updater, the macOS paths, controllers, system link with Linux, Windows and Android machines, the README for macOS. | A release zip from CI. A system link game with all four platforms. | 2–3 weeks |
| 4. Optional | A native Metal renderer. Bink video. | Only if Phase 2's profile shows that it is necessary. | 6–10 weeks |

## Risks

| Risk | Effect | What to do |
| --- | --- | --- |
| The second stage of address translation slows the game | The game's memory access is slower than native. Measured worst case with 2 MB blocks: +14% at 256 MB of random access. | Map with 2 MB blocks. Phase 0 measures the game. |
| Frequent host calls that this proposal did not find | Each is an exit of ~0.75 µs | Count the exits by import number in the host in Phase 1. Move the frequent ones into the guest, as for the thread pointer and the clock. |
| Synchronous GL calls in the frame | The ring flushes too often, and the game waits for the render thread | Count the flushes in Phase 2. The design above leaves one wait for each frame. |
| Host writes into watched memory | A texture is not uploaded again after the host writes it | Record the host's writes in the watch (refer to "The host"). |
| ANGLE's Metal backend has a defect, or a future ANGLE drops something | Rendering errors | The ES 3.0 paths already operate on Android. Pin the ANGLE version. Apple's GL 4.1 is a second path, but it is deprecated. |
| Apple restricts Hypervisor.framework | No port | It is a public, documented framework. QEMU (and thus UTM) uses it directly. The risk is low. |
| Debugging in the guest | lldb cannot see guest code | The host prints the registers and the frame chain, as Android does (`llvm-symbolizer`). Later: a GDB stub on the vCPU's debug exceptions. |
| No Hypervisor.framework on CI runners | No automatic tests of the game on macOS | Build in CI. Test on a Mac, or on a self-hosted runner. |

## What changes outside `port/macos`

Few changes, so upstream can review them:

- `tools/android_imports.py`, `tools/android_gl_stubs.py`,
  `tools/android_posix_stubs.py`: a mode for `hvc` stubs, for pointer
  changes on the host side, for `errno` in the same exit, and for encoding
  GL stubs.
- `port/android/guest/libc` and `runtime`: the thread pointer in
  `TPIDR_EL0` and `clock_gettime` from `CNTVCT_EL0`, for the macOS guest.
- `tools/macos_build.py` (new), and `configure.py` for the target.
- `port/linux/src/posix_*.c`: `#ifdef __APPLE__` for the Darwin differences
  above. These files are host code on Android and on macOS.
- `port/linux/src/d3d8_gl.c`: `base_vertex` from the extension; the split
  of `HALO_ANDROID` (refer to "The guest").
- `port/android/README.md`: the widescreen text (refer to "Graphics").
- No change in `source/`, except possibly the name of the switch at its 18
  `HALO_ANDROID` lines.

## Open questions

These are for the project, not for this proposal:

1. Does the project pay for a Developer ID and notarization, so that
   Gatekeeper does not stop the app? Without it, each new user must allow
   the app by hand.
2. What is the oldest macOS to support? The Hypervisor.framework calls that
   `hvf_*.c` use exist since macOS 11. `hv_vm_config_*` needs macOS 13.
3. Is a split of `HALO_ANDROID` into `HALO_GUEST`, `HALO_GLES` and
   `HALO_ANDROID` acceptable? It touches many files of `port/linux/src`.
4. Somebody with the game data: what are `stats.draws` and the GL calls
   per frame in a heavy scene? That number decides if the command ring is
   needed in Phase 1 or Phase 2.

## Measurements: how to repeat them

```bash
cd port/macos/measurements
clang -O2 low_memory.c -o low_memory && ./low_memory
clang -O2 hvf_exit.c -o hvf_exit -framework Hypervisor
codesign -s - --entitlements hypervisor.entitlements -f hvf_exit && ./hvf_exit
clang hvf_limits.c -o hvf_limits -framework Hypervisor
codesign -s - --entitlements hypervisor.entitlements -f hvf_limits && ./hvf_limits
clang -O2 hvf_mmu.c -o hvf_mmu -framework Hypervisor
codesign -s - --entitlements hypervisor.entitlements -f hvf_mmu && ./hvf_mmu
clang -O2 angle_caps.c -o angle_caps
./angle_caps "/Applications/Visual Studio Code.app/Contents/Frameworks/Electron Framework.framework/Libraries"
```

`angle_caps` needs a folder with `libEGL.dylib` and `libGLESv2.dylib`. Any
Electron app has one. Add results from other Macs (M2, M3, M4) to the
tables above.
