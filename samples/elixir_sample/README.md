# LibGodotConnector (Elixir)

This is a proof-of-concept showing how to interface Elixir with LibGodot using NIFs.

## Architecture

1.  **Elixir to Godot:** The `LibGodot` module provides functions like `start/0` and `iteration/0` which call into the C++ NIF.
	It also provides `send_message/2`, which enqueues messages for Godot to consume via the `ElixirBus` singleton.
2.  **Godot to Elixir:** Godot can call `ElixirBus.send_event(kind, payload)` to push events back to the subscribed Elixir process.
	You can "subscribe" a process using `LibGodot.subscribe(self())`.

## How to build

1.  Ensure you have built `libgodot` in the root directory.
2.  Build the NIF via Mix (recommended):

```bash
cd samples/elixir_sample
mix deps.get
mix compile
```

This uses `elixir_make` and the local `Makefile` to drive a CMake build.

### Optional: use precompiled NIFs from GitHub Releases

If you publish precompiled artefacts (tarballs) to GitHub Releases for the current version,
`mix compile` can download them instead of building locally.

```bash
cd samples/elixir_sample
mix compile
```

To override the default URL template, set `LIBGODOT_PRECOMPILED_URL` to a template containing
`@{artefact_filename}`.

To force a local build (skip download attempts):

```bash
export LIBGODOT_FORCE_BUILD=1
mix compile
```

3.  Alternatively, build the NIF with CMake directly:

```bash
cd samples/elixir_sample
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For a debug build:

```bash
cd samples/elixir_sample
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

4.  Run the Elixir code:

```bash
mix compile
iex -S mix
```

To run it as a normal OTP application (starts `LibGodot.Driver` under supervision):

```bash
mix run --no-halt
```

## Example Usage

```elixir
# In IEx
LibGodot.subscribe(self())

{:ok, godot} =
	LibGodot.create([
		"godot",
		"--headless",
		"--quit"
	])

# If you want to be explicit about which libgodot to load:
# {:ok, godot} = LibGodot.create("../../build/libgodot.dylib", ["godot", "--headless"])  # macOS
# {:ok, godot} = LibGodot.create("../../build/libgodot.so", ["godot", "--headless"])     # Linux

LibGodot.start(godot)

# Run a few frames (or drive this from a GenServer timer)
LibGodot.iteration(godot)
LibGodot.iteration(godot)

LibGodot.shutdown(godot)

# Wait for message
flush()
# Should see: {:godot_status, :started}
```

## Driving `iteration/1` from Elixir

For convenience, this sample includes a tiny GenServer that owns the Godot instance
and calls `iteration/1` on a timer:

```elixir
iex -S mix

{:ok, _pid} =
	LibGodot.Driver.start_link(
		interval_ms: 16,
		notify_pid: self()
	)

# You should see messages like:
# {:godot_status, :started}
flush()

# Send a message into Godot (available via Engine.get_singleton("ElixirBus") in scripts)
:ok = LibGodot.Driver.send_message("hello from Elixir")

# Request/reply: sends a request into Godot and blocks waiting for a response.
# Godot must call ElixirBus.respond(request_id, response) for this to complete.
{:ok, resp} = LibGodot.Driver.request("ping", 1_000)
IO.inspect(resp)

# Receive events sent from Godot via ElixirBus.send_event/2
LibGodot.subscribe(self())
flush()
```

On macOS, embedding Godot inside the BEAM is only supported in headless mode.
The driver defaults to `--headless` and the native layer disables AppKit initialization when it detects `--headless`.
You can override by passing `args: [...]`, but windowed mode may crash due to AppKit main-thread requirements.

Note: the sample project lives at `samples/project/`, so from `samples/elixir_sample/` the path is `../project/`.
