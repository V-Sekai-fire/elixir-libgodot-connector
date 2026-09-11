defmodule LibGodotConnector.MixProject do
  use Mix.Project

  @version "4.5.1-5"
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
      compilers: Mix.compilers() ++ [:elixir_make],
      make_clean: ["clean"],
      deps: deps(),
      releases: releases()
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
      {:ex_doc, "~> 0.34", only: :dev, runtime: false},
      # Burrito is only pulled in the :prod release path; on hex it is
      # {:burrito, "~> 1.6"} as of 2026. Keep it out of :dev/:test so
      # the connector's own unit tests do not need it installed.
      {:burrito, "~> 1.6", only: [:prod, :dev], runtime: false}
    ]
  end

  # ---------------------------------------------------------------------
  # Burrito release: BEAM release + libgodot_host + libgodot.dylib +
  # libiceoryx2_ffi_c.dylib bundled per platform.
  #
  # The three native paths come from env vars set by CI (or the
  # operator) — mix.exs stays agnostic of where they were built.
  # Local `mix release --overwrite` without them still produces a
  # release; it just is not self-contained.
  defp releases do
    [
      lib_godot_connector: [
        steps: [:assemble, &stage_native_libs/1, &Burrito.wrap/1],
        burrito: [
          targets: [
            macos_arm64: [os: :darwin, cpu: :aarch64],
            macos_x86_64: [os: :darwin, cpu: :x86_64],
            linux_x86_64: [os: :linux, cpu: :x86_64]
          ]
        ]
      ]
    ]
  end

  # Copy the three native artifacts named by the LIBGODOT_HOST,
  # LIBGODOT_LIB, and ICEORYX2_LIB env vars into the release's
  # priv/ directory. Silently skips any missing var so a developer
  # can produce a partial release for inspection without breaking.
  defp stage_native_libs(%Mix.Release{} = release) do
    priv = Path.join(release.path, "lib/lib_godot_connector-#{release.version}/priv")
    File.mkdir_p!(priv)

    for env <- ~w(LIBGODOT_HOST LIBGODOT_LIB ICEORYX2_LIB),
        path = System.get_env(env),
        is_binary(path) do
      cond do
        File.exists?(path) ->
          File.cp!(path, Path.join(priv, Path.basename(path)))
          IO.puts("stage_native_libs: copied #{path}")

        true ->
          IO.warn("stage_native_libs: #{env}=#{path} does not exist; skipping")
      end
    end

    release
  end

  defp precompiled_opts do
    force_build? = System.get_env("LIBGODOT_FORCE_BUILD") in ["1", "true", "TRUE"]

    url = System.get_env("LIBGODOT_PRECOMPILED_URL") || @default_precompiled_url

    priv_paths =
      case :os.type() do
        {:win32, _} -> ["libgodot_nif.dll", "weft_client_nif.dll", "libgodot.*"]
        _ -> ["libgodot_nif.so", "weft_client_nif.so", "libgodot.*"]
      end

    [
      # Fetch precompiled NIFs from GitHub releases.
      # If unavailable, elixir_make falls back to building locally.
      make_precompiler: {:nif, LibGodotConnector.Precompiler},
      make_precompiler_url: url,
      # When true, skip any download attempts and always build locally.
      make_force_build: force_build?,
      # Our actual NIF filename is libgodot_nif.so (not derived from app name).
      make_precompiler_filename: "libgodot_nif",
      # Include the NIF and the packaged libgodot next to it.
      make_precompiler_priv_paths: priv_paths
    ]
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
      "LICENSE",
      "README.md",
      "mix.exs"
    ]

    # Only required when shipping precompiled NIFs.
    files =
      files ++ Enum.filter(["checksum.exs"], &File.exists?/1)

    [
      name: "lib_godot_connector",
      licenses: ["MIT"],
      links: %{
        "GitHub" => "https://github.com/#{@github_repo}"
      },
      files: files
    ]
  end
end
