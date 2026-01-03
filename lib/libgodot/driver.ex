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

  def send_message(ref, msg) when is_binary(ref) and is_binary(msg) do
    GenServer.call(LibGodotConnector.Godot, {:send_message, ref, msg})
  end

  def request(ref, msg, timeout_ms \\ 5_000)
      when is_binary(ref) and is_binary(msg) and is_integer(timeout_ms) and timeout_ms >= 0 do
    GenServer.call(LibGodotConnector.Godot, {:request, ref, msg, timeout_ms}, timeout_ms + 1_000)
  end

  def iteration(ref) when is_binary(ref) do
    GenServer.call(LibGodotConnector.Godot, {:iteration, ref})
  end

  def create(args, lib_path \\ nil) do
    GenServer.call(LibGodotConnector.Godot, {:create, args, lib_path})
  end

  def start(ref) do
    GenServer.call(LibGodotConnector.Godot, {:start, ref})
  end

  def shutdown(ref) do
    GenServer.call(LibGodotConnector.Godot, {:shutdown, ref})
  end

  @impl true
  def init(opts) do
    args = Keyword.get(opts, :args, default_args())
    interval_ms = Keyword.get(opts, :interval_ms, @default_interval)

    if notify = opts[:notify_pid] do
      :ok = GenServer.call(LibGodotConnector.Port, {:subscribe, notify})
    end

    create_result =
      case opts[:libgodot_path] do
        nil -> GenServer.call(LibGodotConnector.Port, {:create, args, nil})
        path -> GenServer.call(LibGodotConnector.Port, {:create, args, path})
      end

    with {:ok, godot_ref} <- create_result,
         :ok <- GenServer.call(LibGodotConnector.Port, {:start, godot_ref}) do
      state = %{godot_ref: godot_ref, interval_ms: interval_ms}
      Process.send_after(self(), :tick, interval_ms)
      {:ok, state}
    else
      {:error, reason} ->
        require Logger
        Logger.error("Failed to start Godot: #{inspect(reason)}")
        Logger.error("Args used: #{inspect(args)}")
        {:stop, {:godot_error, reason}}
      other ->
        require Logger
        Logger.error("Unexpected error starting Godot: #{inspect(other)}")
        {:stop, {:unexpected, other}}
    end
  end

  @impl true
  def handle_info(:tick, %{godot_ref: godot_ref, interval_ms: interval_ms} = state) do
    case GenServer.call(LibGodotConnector.Port, {:iteration, godot_ref}) do
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
  def handle_call({:send_message, ref, msg}, _from, state) do
    case GenServer.call(LibGodotConnector.Port, {:send_message, ref, msg}) do
      :ok ->
        {:reply, :ok, state}

      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end

  @impl true
  def handle_call({:request, ref, msg, timeout_ms}, _from, state) do
    # For now, requests are just sent as messages - no response waiting implemented
    case GenServer.call(LibGodotConnector.Port, {:request, ref, msg, timeout_ms}) do
      :ok -> {:reply, {:ok, "message_sent"}, state}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def handle_call({:iteration, ref}, _from, state) do
    case GenServer.call(LibGodotConnector.Port, {:iteration, ref}) do
      :ok -> {:reply, :ok, state}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def handle_call({:create, args, lib_path}, _from, state) do
    case GenServer.call(LibGodotConnector.Port, {:create, args, lib_path}) do
      {:ok, ref} -> {:reply, {:ok, ref}, state}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def handle_call({:start, ref}, _from, state) do
    case GenServer.call(LibGodotConnector.Port, {:start, ref}) do
      :ok -> {:reply, :ok, state}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def handle_call({:shutdown, ref}, _from, state) do
    case GenServer.call(LibGodotConnector.Port, {:shutdown, ref}) do
      :ok -> {:reply, :ok, state}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def handle_call({:subscribe, pid}, _from, state) do
    # Forward subscribe to port
    case GenServer.call(LibGodotConnector.Port, {:subscribe, pid}) do
      :ok -> {:reply, :ok, state}
      error -> {:reply, error, state}
    end
  end

  @impl true
  def terminate(_reason, %{godot_ref: godot_ref}) do
    _ = GenServer.call(LibGodotConnector.Port, {:shutdown, godot_ref})
    :ok
  end

  defp default_args do
    project_path =
      case File.cwd() do
        {:ok, cwd} ->
          Path.expand("samples/project", cwd)
        _ ->
          # Fallback to relative path if cwd fails
          Path.expand("samples/project")
      end

    # Verify the path exists
    unless File.exists?(project_path) do
      require Logger
      Logger.error("Godot project path does not exist: #{project_path}")
      Logger.error("Current working directory: #{inspect(File.cwd())}")
    end

    args = [
      "godot",
      "--path",
      project_path
    ]

    require Logger
    Logger.debug("Starting Godot with args: #{inspect(args)}")

    args
  end
end
