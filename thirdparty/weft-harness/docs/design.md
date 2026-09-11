# How the harness reaches iceoryx2

Moved out of `README.md` when this repository was listed in the fabric manifest, which
holds a README this project is the primary source for to under forty lines. The
convention says to move the content rather than delete it; nothing here was rewritten.

## Nothing links iceoryx2

`iceoryx2.sigs` lists the 40 C ABI functions the harness calls. Chromium's
`generate_stubs.py`, vendored at `thirdparty/generate_stubs`, turns that list
into a dlsym dispatch table. The pattern comes from `entities-godot`, which uses it for
GStreamer.

So the harness builds on a machine that has never seen iceoryx2, and it fails at start
rather than at link when the library is absent. `ldd` on either binary lists no iceoryx2.

That matters more here than it did for GStreamer. iceoryx2 is Rust, and weft writes no
Rust. A dlopen keeps the Rust artifact out of weft's build graph as well as out of its
source.

Three generated pieces come from that one file, and each has a reason.

- `iceoryx2_stubs.cc`, the dispatch table.
- `iox2_api.h`, the prototypes. `generate_stubs.py` emits none, because Chromium's callers
  include the real library headers. A prototype that disagrees with the table it calls
  through is a crash with no diagnostic, so both come from the same file.
- `src/iox2_decls.h` is hand-written, and it is the one piece that is not generated. It
  declares the opaque types. A handle is a pointer, so an incomplete struct is exact. A
  storage struct, `iox2_..._t`, is a real sized struct, and transcribing it would be a
  silent memory bug the day upstream adds a field. It stays incomplete, and the harness
  passes NULL for every one. iceoryx2 then allocates on the heap, which is the documented
  contract.

## Why iceoryx2 and not iceoryx v1

iceoryx v2.0.8, which is the C++ project, does not build here. Its Linux platform layer
includes `<sys/acl.h>` and links `acl`, and libacl is not allowed.

Neither part can be turned off from the command line. `LINUX` is a normal CMake variable
that shadows the cache, so `-DLINUX=OFF` does nothing, and `ICEORYX_PLATFORM` is a
`CACHE PATH FORCE`. Upstream ships an ACL-free `unix` layer, and reaching it needs a patch
to two build files.

weft's `docs/essays/runtime-choice.md` holds the full reversal, and the cost of it.
