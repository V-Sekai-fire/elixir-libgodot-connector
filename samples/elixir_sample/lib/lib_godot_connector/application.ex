defmodule LibGodotConnector.Application do
  use Application

  @impl true
  def start(_type, _args) do
    children = [
      %{
        id: LibGodotConnector.Godot,
        start: {LibGodot.Driver, :start_link, [[name: LibGodotConnector.Godot, interval_ms: 16]]},
        restart: :transient
      },
      %{
        id: LibGodotConnector.EventReceiver,
        start: {LibGodotConnector.EventReceiver, :start_link, [[name: LibGodotConnector.EventReceiver]]},
        restart: :permanent
      },
      %{
        id: LibGodotConnector.Pinger,
        start:
          {LibGodotConnector.Pinger, :start_link,
           [[name: LibGodotConnector.Pinger, server: LibGodotConnector.Godot, interval_ms: 1_000]]},
        restart: :permanent
      }
    ]

    Supervisor.start_link(children, strategy: :one_for_one, name: LibGodotConnector.Supervisor)
  end
end
