defmodule LibGodotConnector.Precompiler do
  @moduledoc false

  @behaviour ElixirMake.Precompiler

  @supported_fetch_targets [
    # Matches what we build/release today.
    "x86_64-linux-gnu",
    "aarch64-apple-darwin",
    # Common Rosetta/Intel macOS case.
    "x86_64-apple-darwin"
  ]

  @impl true
  def current_target do
    system_arch = to_string(:erlang.system_info(:system_architecture))
    parts = String.split(system_arch, "-", trim: true)

    case parts do
      # e.g. x86_64-pc-linux-gnu -> x86_64-linux-gnu
      [arch, _vendor, os, abi] ->
        {:ok, "#{arch}-#{os}-#{abi}"}

      # e.g. aarch64-apple-darwin23.2.0 -> aarch64-apple-darwin
      [arch, os, abi] ->
        abi =
          if String.starts_with?(abi, "darwin") do
            "darwin"
          else
            abi
          end

        {:ok, "#{arch}-#{os}-#{abi}"}

      _ ->
        {:error, "cannot decide current target from #{system_arch}"}
    end
  end

  @impl true
  def all_supported_targets(:compile) do
    case current_target() do
      {:ok, current} -> [current]
      _ -> []
    end
  end

  @impl true
  def all_supported_targets(:fetch), do: @supported_fetch_targets

  @impl true
  def build_native(args), do: ElixirMake.Precompiler.mix_compile(args)

  @impl true
  def precompile(args, _target) do
    ElixirMake.Precompiler.mix_compile(args)
    :ok
  end
end
