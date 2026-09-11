defmodule LibGodot.Driver do
  use GenServer

  @type opt ::
          {:args, [String.t()]}
          | {:libgodot_path, String.t()}
          | {:interval_ms, pos_integer()}
          | {:notify_pid, pid()}

  @default_interval 16

  def start_link(opts) when is_list(opts) do
    GenServer.start_link(__MODULE__, opts, name: opts[:name])
  end

  def send_message(server \\ __MODULE__, msg) when is_binary(msg) do
    GenServer.call(server, {:send_message, msg})
  end

  def request(server \\ __MODULE__, msg, timeout_ms \\ 5_000)
      when is_binary(msg) and is_integer(timeout_ms) and timeout_ms >= 0 do
    GenServer.call(server, {:request, msg, timeout_ms}, timeout_ms + 1_000)
  end

  @impl true
  def init(opts) do
    args = Keyword.get(opts, :args, default_args())
    interval_ms = Keyword.get(opts, :interval_ms, @default_interval)

    if notify = opts[:notify_pid] do
      _ = LibGodot.subscribe(notify)
    end

    create_result =
      case opts[:libgodot_path] do
        nil -> LibGodot.create(args)
        path -> LibGodot.create(path, args)
      end

    with {:ok, godot} <- create_result,
         :ok <- LibGodot.start(godot) do
      state = %{godot: godot, interval_ms: interval_ms}
      Process.send_after(self(), :tick, interval_ms)
      {:ok, state}
    else
      {:error, reason} -> {:stop, {:godot_error, reason}}
      other -> {:stop, {:unexpected, other}}
    end
  end

  @impl true
  def handle_info(:tick, %{godot: godot, interval_ms: interval_ms} = state) do
    case LibGodot.iteration(godot) do
      :ok ->
        Process.send_after(self(), :tick, interval_ms)
        {:noreply, state}

      {:error, "quit"} ->
        {:stop, :normal, state}

      {:error, reason} ->
        {:stop, {:iteration_failed, reason}, state}
    end
  end

  @impl true
  def handle_call({:send_message, msg}, _from, %{godot: godot} = state) do
    case LibGodot.send_message(godot, msg) do
      :ok ->
        {:reply, :ok, state}

      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end

  @impl true
  def handle_call({:request, msg, timeout_ms}, _from, %{godot: godot} = state) do
    {:reply, LibGodot.request(godot, msg, timeout_ms), state}
  end

  @impl true
  def terminate(_reason, %{godot: godot}) do
    _ = LibGodot.shutdown(godot)
    :ok
  end

  defp default_args do
    [
      "godot",
      "--headless",
      "--path",
      "project"
    ]
  end
end
