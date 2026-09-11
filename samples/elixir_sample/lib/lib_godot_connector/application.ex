defmodule LibGodotConnector.Application do
  @moduledoc """
  OTP application entry for `:lib_godot_connector`.

  The connector is a library — the dependent app supervises its own
  `Weft.Client` and `LibGodot.Driver` GenServers. The supervision tree
  here is empty on purpose so `mix.exs`'s `:mod` declaration boots
  cleanly (Application behaviour requires a `start/2` callback).
  """
  use Application

  @impl true
  def start(_type, _args) do
    Supervisor.start_link([], strategy: :one_for_one, name: LibGodotConnector.Supervisor)
  end
end
