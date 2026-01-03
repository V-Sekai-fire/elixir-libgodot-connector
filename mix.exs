defmodule LibGodotConnector.MixProject do
  use Mix.Project

  @version "4.3.1"
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
      compilers: Mix.compilers(),
      deps: deps()
    ]
  end

  def application do
    [
      extra_applications: [:logger],
      mod: {LibGodotConnector.Application, []}
    ]
  end

  defp deps do
    [
      {:jason, "~> 1.4"},
      {:ex_doc, "~> 0.34", only: :dev, runtime: false}
    ]
  end


  defp description do
    "Elixir connector for Godot via port-based communication."
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


    [
      name: "lib_godot_connector",
      licenses: ["MIT"],
      files: files
    ]
  end
end
