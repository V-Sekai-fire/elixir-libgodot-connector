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
      end) ||
        find_nested_nif_path_no_ext(base)

    case nif_path do
      nil ->
        {:error, {:nif_not_found, candidates}}

      path_no_ext ->
        :erlang.load_nif(to_charlist(path_no_ext), 0)
    end
  end

  defp find_nested_nif_path_no_ext(base) do
    # elixir_make extracts archives into priv/, and depending on how the tar.gz was
    # created, it may include a top-level folder. Support that by searching under
    # the app priv directory.
    priv_dir = Application.app_dir(:lib_godot_connector, "priv")

    pattern = Path.join(priv_dir, "**/#{base}.so")

    case Path.wildcard(pattern) do
      [first | _] ->
        String.replace_suffix(first, ".so", "")

      [] ->
        nil
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
