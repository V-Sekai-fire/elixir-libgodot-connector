defmodule Weft.Client do
  @moduledoc """
  Elixir side of the weft::harness / iceoryx2 lifecycle channel.

  The BEAM manages the lifecycle of one or many `libgodot_host` processes
  and never sits on the hot path. Commands travel byte-slice-in,
  byte-slice-out over shared memory (see
  `thirdparty/weft-harness/include/weft/command.hpp` for the framing);
  every request/reply pair is correlated by an 8-byte request id that
  the NIF prepends on send and strips on receive.

  `iceoryx2` is dlopen'd at runtime — the NIF calls `weft::load_bus`
  which honors `WEFT_ICEORYX2_PATH` and falls back to the loader path.
  If the bus cannot be loaded `open/0` returns `{:error, :bus_unreachable}`.

  ## Opcode table (matches the Ask handler in `samples/libgodot_host/host.cpp`)

  | opcode | name    | request body                       | reply body                           |
  | ------ | ------- | ---------------------------------- | ------------------------------------ |
  | 0x01   | CREATE  | NUL-separated argv (empty = default) | ok | err(reason)                    |
  | 0x02   | START   | -                                  | ok | err(reason)                     |
  | 0x03   | ITERATE | -                                  | ok, quit:u8, tick:u64 LE             |
  | 0x04   | STOP    | -                                  | ok                                   |
  | 0x05   | DESTROY | -                                  | ok (idempotent)                      |
  | 0x7F   | QUIT    | -                                  | ok, then the host loop ends          |

  The first byte of every reply is the result code: `0x00` = ok,
  `0x01` = err (followed by a UTF-8 reason). `iterate/2` unpacks the
  quit-flag and tick counter for callers.
  """

  defmodule NIF do
    @moduledoc false
    @on_load :load_nif

    def load_nif do
      priv = :code.priv_dir(:lib_godot_connector)
      :erlang.load_nif(:filename.join(priv, ~c"weft_client_nif"), 0)
    end

    def open, do: :erlang.nif_error(:nif_not_loaded)
    def call(_ref, _body, _timeout_ms), do: :erlang.nif_error(:nif_not_loaded)
    def close(_ref), do: :erlang.nif_error(:nif_not_loaded)
  end

  @typedoc "Opaque bus handle. Do not depend on its shape — the NIF resource is what matters."
  @opaque handle :: reference()

  @doc """
  Open the bus. Fails with `:bus_unreachable` when iceoryx2 is not on the
  loader path or `WEFT_ICEORYX2_PATH` does not point at a real library.
  """
  @spec open() :: {:ok, handle()} | {:error, atom()}
  def open, do: NIF.open()

  @spec close(handle()) :: :ok
  def close(ref), do: NIF.close(ref)

  # ---------------------------------------------------------------------
  # Opcode helpers. Each returns the raw {:ok, reply_body} | {:error, atom}
  # from the NIF; caller-friendly wrappers decode the reply's first byte.

  @doc "Send an opcode with an optional payload. Returns the raw reply body."
  @spec send_opcode(handle(), byte(), binary(), non_neg_integer()) ::
          {:ok, binary()} | {:error, atom()}
  def send_opcode(ref, opcode, payload \\ <<>>, timeout_ms \\ 5_000)
      when is_integer(opcode) and opcode in 0..255 do
    NIF.call(ref, <<opcode::8, payload::binary>>, timeout_ms)
  end

  @doc "Boot the engine on the host. `argv_bytes` is NUL-separated; empty uses host's --script/--project defaults."
  @spec create(handle(), binary(), non_neg_integer()) :: :ok | {:error, atom() | binary()}
  def create(ref, argv_bytes \\ <<>>, timeout_ms \\ 30_000) do
    decode_ok_or_err(send_opcode(ref, 0x01, argv_bytes, timeout_ms))
  end

  @spec start(handle(), non_neg_integer()) :: :ok | {:error, atom() | binary()}
  def start(ref, timeout_ms \\ 10_000) do
    decode_ok_or_err(send_opcode(ref, 0x02, <<>>, timeout_ms))
  end

  @doc """
  Ask the host to run one engine iteration.
  Returns `{:ok, %{quit: boolean(), tick: non_neg_integer()}}` on success.
  """
  @spec iterate(handle(), non_neg_integer()) :: {:ok, map()} | {:error, atom() | binary()}
  def iterate(ref, timeout_ms \\ 5_000) do
    case send_opcode(ref, 0x03, <<>>, timeout_ms) do
      {:ok, <<0x00, quit::8, tick::little-unsigned-64>>} ->
        {:ok, %{quit: quit == 1, tick: tick}}

      {:ok, <<0x01, reason::binary>>} ->
        {:error, reason}

      {:ok, other} ->
        {:error, {:malformed_reply, other}}

      {:error, _} = e ->
        e
    end
  end

  @spec stop(handle(), non_neg_integer()) :: :ok | {:error, atom() | binary()}
  def stop(ref, timeout_ms \\ 10_000) do
    decode_ok_or_err(send_opcode(ref, 0x04, <<>>, timeout_ms))
  end

  @spec destroy(handle(), non_neg_integer()) :: :ok | {:error, atom() | binary()}
  def destroy(ref, timeout_ms \\ 10_000) do
    decode_ok_or_err(send_opcode(ref, 0x05, <<>>, timeout_ms))
  end

  @doc "Tell the host to tear down and exit its loop."
  @spec quit(handle(), non_neg_integer()) :: :ok | {:error, atom() | binary()}
  def quit(ref, timeout_ms \\ 10_000) do
    decode_ok_or_err(send_opcode(ref, 0x7F, <<>>, timeout_ms))
  end

  defp decode_ok_or_err({:ok, <<0x00, _rest::binary>>}), do: :ok
  defp decode_ok_or_err({:ok, <<0x01, reason::binary>>}), do: {:error, reason}
  defp decode_ok_or_err({:ok, other}), do: {:error, {:malformed_reply, other}}
  defp decode_ok_or_err({:error, _} = e), do: e
end
