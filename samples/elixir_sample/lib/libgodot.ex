defmodule LibGodot do
  @on_load :load_nif

  def load_nif do
    # Important: :erlang.load_nif/2 expects the path *without* the platform extension
    # (it appends .so/.dylib/.dll internally).
    base = "libgodot_nif"

    candidates = [
      Application.app_dir(:lib_godot_connector, "priv/#{base}"),
      Path.join([File.cwd!(), "priv", base])
    ]

    nif_path =
      Enum.find(candidates, fn path_no_ext ->
        File.exists?(path_no_ext <> ".so")
      end)

    case nif_path do
      nil ->
        {:error, {:nif_not_found, candidates}}

      path_no_ext ->
        :erlang.load_nif(to_charlist(path_no_ext), 0)
    end
  end

  def subscribe(_pid) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def send_message(_ref, _msg) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def request(_ref, _msg, _timeout_ms) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def create(_args) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def create(_libgodot_path, _args) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def start(_ref) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def iteration(_ref) do
    :erlang.nif_error(:nif_not_loaded)
  end

  def shutdown(_ref) do
    :erlang.nif_error(:nif_not_loaded)
  end
end
