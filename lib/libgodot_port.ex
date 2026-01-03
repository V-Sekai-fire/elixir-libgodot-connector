defmodule LibGodot.Port do
  @moduledoc """
  Port-based implementation of LibGodot that runs Godot in a separate process.
  This avoids threading/mutex issues by running Godot on its own main thread.
  """
  
  use GenServer
  
  defstruct [:port, :ref, :subscriber]
  
  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, opts, name: opts[:name] || __MODULE__)
  end
  
  def create(args) do
    GenServer.call(__MODULE__, {:create, args, nil})
  end
  
  def create(lib_path, args) do
    GenServer.call(__MODULE__, {:create, args, lib_path})
  end
  
  def start(ref) do
    GenServer.call(__MODULE__, {:start, ref})
  end
  
  def iteration(ref) do
    GenServer.call(__MODULE__, {:iteration, ref})
  end
  
  def shutdown(ref) do
    GenServer.call(__MODULE__, {:shutdown, ref})
  end
  
  def send_message(ref, msg) do
    GenServer.call(__MODULE__, {:send_message, ref, msg})
  end
  
  def subscribe(pid) do
    GenServer.call(__MODULE__, {:subscribe, pid})
  end
  
  @impl true
  def init(_opts) do
    port_path = find_port_executable()
    port = Port.open({:spawn_executable, port_path}, [
      {:args, []},
      :binary,
      :use_stdio,
      :exit_status
      # Don't redirect stderr to stdout - we'll handle it separately
    ])
    
    {:ok, %__MODULE__{port: port, ref: nil, subscriber: nil}}
  end
  
  @impl true
  def handle_call({:create, args, lib_path}, _from, state) do
    cmd = if lib_path do
      Jason.encode!(%{cmd: "create", args: args, lib_path: lib_path})
    else
      Jason.encode!(%{cmd: "create", args: args})
    end
    
    send(state.port, {self(), {:command, cmd <> "\n"}})
    
    response = wait_for_response(state.port)
    
    case response do
      {:ok, %{"ok" => true, "ref" => ref}} ->
        {:reply, {:ok, ref}, %{state | ref: ref}}
      {:ok, %{"ok" => false, "error" => error}} ->
        {:reply, {:error, error}, state}
      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end
  
  @impl true
  def handle_call({:start, ref}, _from, state) do
    cmd = Jason.encode!(%{cmd: "start", ref: ref})
    send(state.port, {self(), {:command, cmd <> "\n"}})
    
    response = wait_for_response(state.port)
    
    case response do
      {:ok, %{"ok" => true}} ->
        {:reply, :ok, state}
      {:ok, %{"ok" => false, "error" => error}} ->
        {:reply, {:error, error}, state}
      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end
  
  @impl true
  def handle_call({:iteration, ref}, _from, state) do
    cmd = Jason.encode!(%{cmd: "iteration", ref: ref})
    send(state.port, {self(), {:command, cmd <> "\n"}})
    
    response = wait_for_response(state.port)
    
    case response do
      {:ok, %{"ok" => true, "quit" => true}} ->
        {:reply, {:error, "quit"}, state}
      {:ok, %{"ok" => true, "quit" => false}} ->
        {:reply, :ok, state}
      {:ok, %{"ok" => false, "error" => error}} ->
        {:reply, {:error, error}, state}
      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end
  
  @impl true
  def handle_call({:shutdown, ref}, _from, state) do
    cmd = Jason.encode!(%{cmd: "shutdown", ref: ref})
    send(state.port, {self(), {:command, cmd <> "\n"}})
    
    response = wait_for_response(state.port)
    
    case response do
      {:ok, %{"ok" => true}} ->
        {:reply, :ok, %{state | ref: nil}}
      {:ok, %{"ok" => false, "error" => error}} ->
        {:reply, {:error, error}, state}
      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end
  
  @impl true
  def handle_call({:request, ref, msg, _timeout_ms}, from, state) do
    # For now, requests are not implemented in the port - just send the message
    handle_call({:send_message, ref, msg}, from, state)
  end

  @impl true
  def handle_call({:send_message, ref, msg}, _from, state) do
    cmd = Jason.encode!(%{cmd: "send_message", ref: ref, msg: msg})
    send(state.port, {self(), {:command, cmd <> "\n"}})

    response = wait_for_response(state.port)

    case response do
      {:ok, %{"ok" => true}} ->
        {:reply, :ok, state}
      {:ok, %{"ok" => false, "error" => error}} ->
        {:reply, {:error, error}, state}
      {:error, reason} ->
        {:reply, {:error, reason}, state}
    end
  end
  
  @impl true
  def handle_call({:subscribe, pid}, _from, state) do
    {:reply, :ok, %{state | subscriber: pid}}
  end
  
  @impl true
  def handle_info({port, {:data, data}}, %{port: port, subscriber: subscriber} = state) do
    # Handle event messages from Godot (lines starting with {"event":...})
    # Skip command responses (they're handled in handle_call via wait_for_response)
    case Jason.decode(data) do
      {:ok, %{"event" => "message", "data" => msg}} when not is_nil(subscriber) ->
        send(subscriber, {:godot_message, msg})
      _ ->
        :ok
    end
    
    {:noreply, state}
  end

  @impl true
  def handle_info({port, {:exit_status, status}}, %{port: port} = state) do
    {:stop, {:port_exit, status}, state}
  end

  # Wait for a command response (not an event)
  defp wait_for_response(port) do
    receive do
      {^port, {:data, {:eol, data}}} ->
        # Handle line-based data (stdout)
        case Jason.decode(data) do
          {:ok, json} when is_map(json) ->
            # Check if it's a command response (has "ok" key) or an event
            if Map.has_key?(json, "ok") do
              {:ok, json}
            else
              # It's an event, wait for the next message
              wait_for_response(port)
            end
          {:error, _error} ->
            # Not JSON - might be stderr or other output, skip it
            wait_for_response(port)
        end
      {^port, {:data, data}} when is_binary(data) ->
        # Handle binary data
        case Jason.decode(data) do
          {:ok, json} when is_map(json) ->
            if Map.has_key?(json, "ok") do
              {:ok, json}
            else
              wait_for_response(port)
            end
          {:error, _} ->
            # Not JSON - might be stderr or other output, skip it
            wait_for_response(port)
        end
    after
      5000 ->
        {:error, "timeout"}
    end
  end

  defp find_port_executable do
    candidates = [
      Application.app_dir(:lib_godot_connector, "priv/libgodot_port"),
      Path.join([File.cwd!(), "priv", "libgodot_port"])
    ]
    
    Enum.find(candidates, &File.exists?/1) || 
      raise "libgodot_port executable not found in #{inspect(candidates)}"
  end
end

