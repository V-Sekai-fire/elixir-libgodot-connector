# One harness, and what is still missing

Moved out of `README.md` when this repository was listed in the fabric manifest, which
holds a README this project is the primary source for to under forty lines. The
convention says to move the content rather than delete it; nothing here was rewritten.

## Where this came from, and what is built

The weft plane harness. Every plane and every edge links this, and none of them links
iceoryx2.

Split out of [`interactor-weft`](https://github.com/v-sekai-multiplayer-fabric/interactor-weft) with its
history. weft keeps only the data plane and the NIF the BEAM loads. A plane is its own
process, its own repository, and its own container.

Goal: one runtime model for every plane. A thin C++ thread-per-core loop over iceoryx2.

State: a first loop exists, compiled against the real ABI, not yet run.

- **Built.** `weft::harness`, a library every plane and edge links. It holds the bus, the
  limits, and the payload type.
- **Proved.** `proof/` passes a `Snapshot` between two processes with no copy and no
  daemon. The run is below.
- **Built and proved on Windows and macOS, not yet on Linux.** `weft/loop.hpp`'s `run_command_loop` —
  a command in, reply bytes out, over a new payload variant (`iox2_type_variant_e_DYNAMIC`,
  a byte slice, not `Snapshot`'s fixed struct) and `weft/command.hpp`'s request-id-prefixed
  envelope. `proof/command_publisher.cpp`/`command_subscriber.cpp` ran for real on Windows
  11 against iceoryx2 v0.9.3 built from source — both exit 0, 8/8 round trips, through the
  actual dlopen stub table this repo generates, not a proxy for it. See "The command/reply
  proof" below for the two real bugs (one iceoryx2's, one MinGW's) that had to be worked
  around to get there, and for what's still not wired into `CMakeLists.txt` as a result.
  It has since run on macOS 26.5.2 arm64 as well — see "The command/reply proof on macOS"
  below, which is where the one defect the Windows run could not have caught was found.
  This repo's primary target is Linux and that run hasn't happened yet.
- One thread, not thread-per-core: the goal above names the eventual shape, and a
  single-process, likely-GPU-bound interactor (one plane, this loop's first intended
  caller) has no per-core work to split. `run_command_loop`, not `run_loop`, so it doesn't
  claim to be that eventual answer.

## One harness, not one for each plane

weft has several planes and two edges, and the number grows. Each one needs the bus and
each one needs the limits.

Left alone, each would grow its own copy of both, and the copies would drift the way a
decision written twice always drifts. That is the failure `Weft.VocabularyTest` was
written for, in a different form.

So there is one. A plane repository brings this in with `git subtree add --prefix=thirdparty/harness`,
and links `weft::harness`.

| what it gives | where                                                 | why it is shared                               |
| ------------- | ----------------------------------------------------- | ---------------------------------------------- |
| the bus       | `iceoryx2.sigs`, and the table generated from it      | one C ABI, one dispatch table                  |
| the limits    | `include/weft/limits.hpp`                             | every value is `Weft.Limits`, which is rivet's |
| the payloads  | `include/weft/snapshot.hpp`, `include/weft/store.hpp` | both ends of a service must agree exactly      |

`Weft.PlaneNetworkingTest` holds that shape. It fails if a second `.sigs` file appears, if
a plane declares a limit of its own, or if a directory with a `CMakeLists.txt` is missing
from the root build.

## What comes next

1. The thread-per-core loop, once, because every plane uses it.
2. iceoryx2 in the container image, so CI runs this proof rather than a person.
3. The first plane behind it. `zone-server-h2o` is the candidate, because its zone tick
   has no input at all until the bus carries one.

## Waiting

`iox2_node_wait` is a periodic sleep. It cannot wake on a packet, so a process holding both a
bus and a socket would need a second event loop for the network, and two event loops in one
process is the thing an edge should not have.

So the harness binds the **WaitSet**. It takes an arbitrary file descriptor through
`iox2_file_descriptor_new`, which accepts a raw `int`, and `iox2_waitset_attach_notification`
returns a guard that identifies which attachment fired. A UDP socket, a `timerfd` and the bus
therefore wait in one place.

The signatures are transcribed from `iceoryx2-ffi/c/src/api` at v0.9.3, which is the C ABI
itself rather than a generated header, because iceoryx2 generates its header at build time and
this repository must build without it.
