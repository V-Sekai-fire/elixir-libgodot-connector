defmodule LibGodotConnector.MixProject do
  use Mix.Project

  @version "4.3.1"
  # Default GitHub repo for precompiled artefacts.
  # Change this to your fork if you publish releases elsewhere.
  @github_repo "Ughuuu/libgodot"
  @default_precompiled_url "https://github.com/#{@github_repo}/releases/download/v#{@version}/@{artefact_filename}"

  def project do
    [
      app: :lib_godot_connector,
      version: @version,
      elixir: "~> 1.14",
      name: "LibGodotConnector",
      description: description(),
      package: package(),
      docs: [main: "readme", extras: ["README.md"]],
      start_permanent: Mix.env() == :prod,
      compilers: [:elixir_make] ++ Mix.compilers(),
      make_clean: ["clean"],
      deps: deps()
    ]
    |> Keyword.merge(precompiled_opts())
  end

  def application do
    [
      extra_applications: [:logger],
      mod: {LibGodotConnector.Application, []}
    ]
  end

  defp deps do
    [
      {:elixir_make, "~> 0.9", runtime: false},
      {:ex_doc, "~> 0.34", only: :dev, runtime: false}
    ]
  end

  defp precompiled_opts do
    force_build? = System.get_env("LIBGODOT_FORCE_BUILD") in ["1", "true", "TRUE"]

    if force_build? do
      []
    else
      url = System.get_env("LIBGODOT_PRECOMPILED_URL") || @default_precompiled_url

      [
        # Fetch precompiled NIFs from GitHub releases.
        # If unavailable, elixir_make falls back to building locally.
        make_precompiler: {:nif, LibGodotConnector.Precompiler},
        make_precompiler_url: url,
        # Our actual NIF filename is libgodot_nif.so (not derived from app name).
        make_precompiler_filename: "libgodot_nif",
        # Include the NIF and the packaged libgodot next to it.
        make_precompiler_priv_paths: ["libgodot_nif.so", "libgodot.*"]
      ]
    end
  end

  defp description do
    "Elixir connector for LibGodot via NIFs (proof-of-concept)."
  end

  defp package do
    files = [
      "lib",
      "src",
      "CMakeLists.txt",
      "Makefile",
      "README.md",
      "mix.exs"
    ]

    # Only required when shipping precompiled NIFs.
    files =
      if File.exists?("checksum-lib_godot_connector.exs") do
        files ++ ["checksum-lib_godot_connector.exs"]
      else
        files
      end

    [
      name: "lib_godot_connector",
      licenses: ["MIT"],
      files: files
    ]
  end
end
