defmodule LibGodotConnector.Pinger do
  use GenServer

  @default_interval_ms 1_000

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, opts, name: opts[:name])
  end

  @impl true
  def init(opts) do
    state = %{
      server: Keyword.get(opts, :server, LibGodotConnector.Godot),
      interval_ms: Keyword.get(opts, :interval_ms, @default_interval_ms),
      n: 0
    }

    Process.send_after(self(), :tick, state.interval_ms)
    {:ok, state}
  end

  @impl true
  def handle_info(:tick, %{server: server, interval_ms: interval_ms, n: n} = state) do
    msg = "ping #{n + 1}"

    t0_us = System.monotonic_time(:microsecond)

    case LibGodot.Driver.request(server, msg, 1_000) do
      {:ok, resp} ->
        dt_us = System.monotonic_time(:microsecond) - t0_us
        IO.puts("[ping_reply] #{resp} (rtt=#{dt_us}us, #{Float.round(dt_us / 1000, 3)}ms)")

      {:error, reason} ->
        dt_us = System.monotonic_time(:microsecond) - t0_us
        IO.puts("[ping_reply_error] #{inspect(reason)} (rtt=#{dt_us}us)")
    end

    Process.send_after(self(), :tick, interval_ms)
    {:noreply, %{state | n: n + 1}}
  end
end
