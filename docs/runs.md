# The proof runs

Moved out of `README.md` when this repository was listed in the fabric manifest, which
holds a README this project is the primary source for to under forty lines. The
convention says to move the content rather than delete it; nothing here was rewritten.

## What the proof is

Two programs and one struct.

- `src/snapshot.hpp` holds `weft::Snapshot`: a tick, an entity, and three positions in
  micrometres. This is the fixed point the data plane already uses.
- `src/publisher.cpp` loans a sample, writes the struct, and sends the loan. It does not
  serialize and it does not copy. `loan_uninit` returns memory the subscriber already
  maps, so a send is a pointer handoff.
- `src/subscriber.cpp` receives and checks. It checks the tick order and the payload it
  derives from the tick.

The check is the point. A bus that delivers garbage must fail here, and not print a count.

## How to run it

The build needs no iceoryx2. The run does. Build and install iceoryx2 v0.9.3 somewhere the
loader can find at run time. The prefix below matches the one `.gitignore` already
excludes.

    cmake -S <iceoryx2-src> -B <iceoryx2-src>/build \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/.iceoryx
    cmake --build <iceoryx2-src>/build -j
    cmake --install <iceoryx2-src>/build

Then build the proof and run both ends. Start the subscriber first.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j

    export LD_LIBRARY_PATH=$PWD/.iceoryx/lib64   # or set WEFT_ICEORYX2_PATH
    ./build/weft-harness-subscriber 8 &
    ./build/weft-harness-publisher 8

## The run

One machine, 16 cores, Fedora, GCC 16.1.1, iceoryx2 v0.9.3, Release.

    publisher: sent 8
    subscriber: tick 1 entity 42 x 1000 z -250
    ...
    subscriber: tick 8 entity 42 x 8000 z -2000
    subscriber: received 8, in order, intact

The subscriber exits 0. No daemon runs, and none is started.

## The command/reply proof (`run_command_loop`) -- run, on Windows

`weft-harness-command_subscriber` runs `weft::run_command_loop` itself (server, echo
`ask`), `weft-harness-command_publisher` sends "ping N" and checks "pong N" comes back on
the reply topic, correlated by request id.

    ./build/weft-harness-command_subscriber 8 &
    ./build/weft-harness-command_publisher 8

**Run for real on Windows 11 Pro (10.0.26200), llvm-mingw g++/clang 22.1.8, iceoryx2 v0.9.3
built from source (`cargo build --release -p iceoryx2-ffi-c`, no prebuilt Windows release
exists).** Two things had to be fixed first, neither in this repository's own code:

1. **iceoryx2 v0.9.3 hardcodes `C:\Temp\` on Windows** and fails with no clear error when
   it doesn't exist on a stock install -- exactly [issue #1868](https://github.com/eclipse-iceoryx/iceoryx2/issues/1868), closed 2026-08-04 (after
   v0.9.3). Fix: `mkdir -p /c/Temp/iceoryx2/shm` before running anything. This is an
   iceoryx2 bug, not a harness one, but every Windows caller will hit it running the exact
   proof below until an iceoryx2 release past that fix exists.
2. **MinGW has no `<dlfcn.h>`**, which `iceoryx2_stubs.cc` needs. Fixed here by vendoring
   [`dlfcn-win32`](https://github.com/dlfcn-win32/dlfcn-win32) v1.4.2 (MIT, implements
   `dlopen`/`dlsym`/`dlclose` over `LoadLibrary`/`GetProcAddress`) at
   `thirdparty/dlfcn-win32/`, included only on Windows. This keeps `posix_stubs`' generated
   code completely unchanged -- no need for Chromium's separate `windows_lib_x64` stub
   type after all, the _code_ the generator already produces just needed a
   `<dlfcn.h>` to compile against.

With both of those, `WEFT_ICEORYX2_PATH` set to the built `iceoryx2_ffi_c.dll`:

    command_publisher: tick 1 ok
    ...
    command_publisher: tick 8 ok
    command_publisher: sent and confirmed 8
    command_subscriber: answered 8, in order

Both exit 0. `run_command_loop`'s actual code path -- `weft::load_bus()`, the dlopen stub
table, `iox2_type_variant_e_DYNAMIC`, `loan_slice_uninit`'s element-count loan, the 8-byte
request-id correlation -- all really ran, not a proxy for it. (A separate same-process
direct-link test against the raw DLL, bypassing the stub table entirely, was written first
as a faster sanity check before wiring up the two-process version above; it also passed
8/8 and is not included here since the two-process run through the real harness code
supersedes it as evidence.)

Two Win32 API warnings appear during the run (`FindNextFileA`/`RemoveDirectoryA`, "no more
files" / "directory is not empty") from iceoryx2's own Windows resource-cleanup path --
cosmetic in this run (both processes still exit 0 with correct results), not investigated
further here, and not new: they're inside iceoryx2 itself, not this repository.

**`dlfcn-win32` is wired into `CMakeLists.txt`** behind `if(WIN32)`, so `cmake -B build -G
Ninja && cmake --build build` on Windows needs no manual flags -- verified with a clean
`Remove-Item -Recurse -Force build` before both the configure and the run above, so this is
what a plane actually gets, not a manual workaround left as prose. One CMake bug found and
fixed getting there: `project(weft-harness CXX)` never enables a C compiler, so
`dlfcn.c` (a C file) got silently dropped from the build with zero errors -- CMake
generated no rule for it at all, and the first sign was `lld-link: error: undefined symbol:
dlopen` at final link, nothing at compile time. Fixed with `enable_language(C)` inside the
`if(WIN32)` block, scoped there since nothing else in this repo is C.

Still open: `WEFT_ICEORYX2_PATH` on Windows needs to point at the `.dll` directly (the
hardcoded fallback names in `bus.hpp`, `libiceoryx2_ffi_c.so`/`.so.0`, are Linux sonames and
don't match a Windows `iceoryx2_ffi_c.dll`) -- no Windows-specific guidance for that is
written down yet the way `LD_LIBRARY_PATH` is documented for Linux above. And the `C:\Temp`
bug is iceoryx2's, not fixable from this repo; every Windows caller needs
`mkdir -p /c/Temp/iceoryx2/shm` (or the Windows equivalent) until an iceoryx2 release past
[#1868](https://github.com/eclipse-iceoryx/iceoryx2/issues/1868) exists.

**Not yet run: on Linux**, which is this project's primary target and everything above was
written for originally. If you run this and it works, replace this paragraph with a real
Linux run log, the same way the
`Snapshot` proof above has one. If it doesn't, that's exactly what this section is for.

`ldd build/weft-harness-publisher` lists no iceoryx2. The library arrives
through `dlopen` at start.

## The command/reply proof on macOS -- run

Both proofs run on **macOS 26.5.2, arm64 (Apple M-series), AppleClang 21.0.0, iceoryx2
v0.9.3 built from source** with `cargo build --release -p iceoryx2-ffi-c`. That build is
clean — no patches, no vendored shims, nothing like the two workarounds Windows needed.
iceoryx2 lists macOS as a supported platform and on this evidence it is one.

    export WEFT_ICEORYX2_PATH=.../iceoryx2/target/release/libiceoryx2_ffi_c.dylib
    ./build/weft-harness-subscriber 8 &          # Snapshot, FIXED_SIZE
    ./build/weft-harness-publisher 8
    ./build/weft-harness-command_subscriber 8 &  # command/reply, DYNAMIC
    ./build/weft-harness-command_publisher 8

    subscriber: received 8, in order, intact
    command_publisher: sent and confirmed 8
    command_subscriber: answered 8, in order

All four exit 0. Both payload variants therefore work here: the fixed 40-byte `Snapshot`
and the byte-slice `loan_slice_uninit` path the command envelope needs.

### The defect this run found

`weft/bus.hpp` listed only `libiceoryx2_ffi_c.so` and `.so.0`, so on macOS the probe could
not succeed however the library was installed — the file cargo produces is
`libiceoryx2_ffi_c.dylib`. The first macOS run passed anyway, because `WEFT_ICEORYX2_PATH`
is prepended to the list and tried first, so setting it skips the names entirely. Windows
was run the same way and never exercised the list either.

So the bug was reachable on two platforms and invisible on both, and only the run that
deliberately _unset_ the override could see it. The fix names the platform's own library
and the platform's own loader variable, and it is checked the only way that means anything:

    unset WEFT_ICEORYX2_PATH
    export DYLD_LIBRARY_PATH=.../iceoryx2/target/release
    ./build/weft-harness-command_publisher 8   -> sent and confirmed 8

The error message named `LD_LIBRARY_PATH` on every platform too, which is advice that does
nothing on a Mac. It now names `DYLD_LIBRARY_PATH`, `PATH` or `LD_LIBRARY_PATH` as the
platform warrants.

`otool -L build/weft-harness-publisher` lists no iceoryx2, the same as `ldd` on Linux. The
library arrives through `dlopen` at start.

## What this run does not measure

The latency and the rate. Eight messages at a 20 ms cycle measures that a message
arrives, and nothing else. A number belongs in `../logbook/data_plane.md` with the machine
and the settings that produced it, and this run produces none.
