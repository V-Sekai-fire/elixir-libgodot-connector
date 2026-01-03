defmodule LibGodotConnector.Application do
  @moduledoc false

  use Application

  @impl true
  def start(_type, _args) do
    children = [
      {LibGodot.Port, name: LibGodotConnector.Port},
      {LibGodot.Driver, name: LibGodotConnector.Godot}
    ]

    opts = [strategy: :one_for_one, name: LibGodotConnector.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
