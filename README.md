# LibGodotConnector (Elixir)

This is a proof-of-concept showing how to interface Elixir with Godot using port-based communication.

## Architecture

1.  **Elixir to Godot:** The `LibGodot.Driver` module provides functions like `create/1`, `start/1`, and `iteration/1` which communicate with a Godot process via stdin/stdout.
	It also provides `send_message/2`, which sends messages to Godot via JSON over the port.
2.  **Godot to Elixir:** Godot can call `ElixirBus.send_event(kind, payload)` to push events back to the subscribed Elixir process via stdout.
	You can "subscribe" a process using `LibGodot.Driver.subscribe/1`.

## How to build

1.  Ensure you have built `libgodot` in the root directory.
2.  Build the port executable via Mix:

```bash
mix deps.get
mix compile
```

This uses `elixir_make` and the local `Makefile` to drive a CMake build of the port executable.

Alternatively, build the port executable with CMake directly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For a debug build:

```bash
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
	LibGodot.Port.create([
		"godot",
		"--path",
		"samples/project"
	])

# The driver automatically starts Godot with a graphical interface
LibGodot.Driver.send_message(godot, "hello from Elixir")

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

Godot runs with a graphical interface by default. The port-based architecture isolates Godot in its own process, avoiding threading issues.
