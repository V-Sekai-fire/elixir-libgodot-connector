defmodule LibGodotConnector.EventReceiver do
  use GenServer

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, opts, name: opts[:name])
  end

  @impl true
  def init(_opts) do
    :ok = LibGodot.subscribe(self())
    {:ok, %{}}
  end

  @impl true
  def handle_info({:godot_event, kind, payload}, state) do
    IO.puts("[godot_event] #{kind}: #{payload}")
    {:noreply, state}
  end

  @impl true
  def handle_info({:godot_message, msg}, state) do
    IO.puts("[godot_message] #{msg}")
    {:noreply, state}
  end

  @impl true
  def handle_info(other, state) do
    IO.inspect(other, label: "[godot_unhandled]")
    {:noreply, state}
  end
end
