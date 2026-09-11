defmodule Weft.ClientTest do
  # Two paired tests per CLAUDE.md rule 2: "A check that passes on
  # known-broken input is decoration — it certifies the defect. Every
  # gate ships with a negative control asserting the broken input fails."
  #
  # Both tests are tagged :bus because they require iceoryx2 to be
  # loadable — test_helper.exs excludes :bus by default, so `mix test`
  # is quiet on machines without the library staged. Enable with:
  #     WEFT_ICEORYX2_PATH=/path/to/libiceoryx2_ffi_c.dylib \
  #       mix test --include bus
  use ExUnit.Case, async: false
  @moduletag :bus

  test "open succeeds when iceoryx2 is on the loader path" do
    # Positive: opening the bus returns {:ok, ref}. Requires
    # WEFT_ICEORYX2_PATH to already point at a real dylib in the
    # ambient env (or the library to be on DYLD/LD_LIBRARY_PATH).
    assert {:ok, ref} = Weft.Client.open()
    assert is_reference(ref)
    :ok = Weft.Client.close(ref)
  end

  test "open fails cleanly when WEFT_ICEORYX2_PATH points at a bogus file" do
    # Control: a broken path returns {:error, :bus_unreachable} (or a
    # near-neighbor error atom). Proves the positive above is not
    # decoration — the check would go red if the NIF trivially always
    # returned {:ok, _}.
    prev = System.get_env("WEFT_ICEORYX2_PATH")
    System.put_env("WEFT_ICEORYX2_PATH", "/nonexistent/libiceoryx2_ffi_c.dylib")

    on_exit(fn ->
      if prev, do: System.put_env("WEFT_ICEORYX2_PATH", prev), else: System.delete_env("WEFT_ICEORYX2_PATH")
    end)

    assert {:error, reason} = Weft.Client.open()
    # Accept a small set of well-known reasons — anything else is a
    # regression that should be looked at, not silently accepted.
    assert reason in [:bus_unreachable, :no_node, :no_service]
  end
end
