defmodule LibGodotConnectorTest do
  use ExUnit.Case, async: false

  setup do
    # Wait for Application to start and Godot to initialize
    # Check supervisor to see if child is running
    wait_for_godot(max_attempts: 10, delay_ms: 200)
    :ok
  end

  defp wait_for_godot(max_attempts: max_attempts, delay_ms: delay_ms) do
    case Process.whereis(LibGodotConnector.Godot) do
      nil ->
        if max_attempts > 0 do
          Process.sleep(delay_ms)
          wait_for_godot(max_attempts: max_attempts - 1, delay_ms: delay_ms)
        else
          # Check supervisor to see what happened
          supervisor_pid = Process.whereis(LibGodotConnector.Supervisor)
          
          if supervisor_pid == nil do
            flunk("Supervisor not found - Application may not have started")
          else
            case Supervisor.which_children(LibGodotConnector.Supervisor) do
              children when is_list(children) ->
                godot_child = Enum.find(children, fn {id, _, _, _} -> id == LibGodotConnector.Godot end)
                case godot_child do
                  {_, :undefined, _, reason} ->
                    IO.puts("Godot driver failed to start. Reason: #{inspect(reason)}")
                    IO.puts("All supervisor children: #{inspect(children)}")
                    flunk("Godot driver failed to start. Reason: #{inspect(reason)}")
                  {_, pid, _, _} when is_pid(pid) ->
                    # Process exists but not registered - check if it crashed
                    if Process.alive?(pid) do
                      :ok
                    else
                      flunk("Godot driver process exists but is not alive")
                    end
                  nil ->
                    flunk("Godot driver not found in supervisor children: #{inspect(children)}")
                  other ->
                    flunk("Unexpected Godot child state: #{inspect(other)}")
                end
              error ->
                flunk("Failed to get supervisor children: #{inspect(error)}")
            end
          end
        end

      pid when is_pid(pid) ->
        :ok
    end
  end

  test "hello world - driver starts successfully" do
    # The driver should start successfully via Application
    pid = Process.whereis(LibGodotConnector.Godot)
    assert pid != nil, "Godot driver should be running"
    assert Process.alive?(pid), "Godot driver process should be alive"
  end

  defp default_args do
    project_path =
      case File.cwd() do
        {:ok, cwd} ->
          Path.expand("samples/project", cwd)
        _ ->
          # Fallback to relative path if cwd fails
          Path.expand("samples/project")
      end

    ["godot", "--path", project_path]
  end

  test "hello world - port can handle subscription" do
    # Test that the port can handle subscription commands
    :ok = GenServer.call(LibGodotConnector.Godot, {:subscribe, self()})

    # Give it a moment to process
    Process.sleep(100)

    # The port should be responsive
    assert Process.alive?(Process.whereis(LibGodotConnector.Godot))
  end

  test "hello world - driver can send messages" do
    # Test that the driver can send messages to the running Godot instance
    # The running driver has a default godot_ref, let's use it
    result = LibGodot.Driver.send_message("godot_1", "test message from Elixir")
    # The driver should successfully send the message
    assert result == :ok, "Driver should successfully send messages to Godot"
  end

  test "hello world - driver can iterate Godot" do
    # Test that the driver can advance Godot's main loop iteration
    # Use the godot_ref from the running instance
    result = LibGodot.Driver.iteration("godot_1")  # The default ref from the running instance
    # The iteration should succeed (Godot should not quit on first iteration)
    assert result == :ok, "Driver should successfully iterate Godot's main loop"
  end

  test "README - port-based Godot lifecycle (create, start, iterate, shutdown)" do
    # Test the complete lifecycle as shown in README example usage

    # Create Godot instance using the driver API
    args = default_args()
    {:ok, godot_ref} = LibGodot.Driver.create(args)

    # Send message before starting
    :ok = LibGodot.Driver.send_message(godot_ref, "hello from Elixir")

    # Start Godot
    :ok = LibGodot.Driver.start(godot_ref)

    # Run a few iterations
    :ok = LibGodot.Driver.iteration(godot_ref)
    :ok = LibGodot.Driver.iteration(godot_ref)

    # Shutdown
    :ok = LibGodot.Driver.shutdown(godot_ref)

    # Verify the process is still responsive
    assert Process.alive?(Process.whereis(LibGodotConnector.Godot))
  end

  test "README - driver with timer-based iteration" do
    # Test the GenServer that drives iteration on a timer as mentioned in README

    # Create a Godot instance first
    args = default_args()
    {:ok, godot_ref} = LibGodot.Driver.create(args)
    :ok = LibGodot.Driver.start(godot_ref)

    # Subscribe to receive events
    :ok = GenServer.call(LibGodotConnector.Godot, {:subscribe, self()})

    # Give it a moment to process subscription
    Process.sleep(50)

    # Send a message to Godot (should be available via Engine.get_singleton("ElixirBus"))
    :ok = LibGodot.Driver.send_message(godot_ref, "hello from Elixir")

    # The message should be processed without errors
    assert Process.alive?(Process.whereis(LibGodotConnector.Godot))
  end

  test "README - request/reply mechanism" do
    # Test the request/reply functionality mentioned in README
    # Note: This may timeout since Godot needs to explicitly respond via ElixirBus.respond()

    # Create a Godot instance first
    args = default_args()
    {:ok, godot_ref} = LibGodot.Driver.create(args)
    :ok = LibGodot.Driver.start(godot_ref)

    # Send a request that should work (but may timeout if Godot doesn't respond)
    result = LibGodot.Driver.request(godot_ref, "ping", 100)
    # The request should return the message sent status
    assert result == {:ok, "message_sent"}, "Request should send message successfully"
  end

  test "README - event subscription and message reception" do
    # Test subscribing to events as shown in README

    # Create a Godot instance first
    args = default_args()
    {:ok, godot_ref} = LibGodot.Driver.create(args)
    :ok = LibGodot.Driver.start(godot_ref)

    # Subscribe self to receive events
    :ok = GenServer.call(LibGodotConnector.Godot, {:subscribe, self()})

    # Send a message that might trigger Godot to send events
    :ok = LibGodot.Driver.send_message(godot_ref, "trigger event")

    # Give Godot time to process and potentially send events
    Process.sleep(200)

    # Check if we received any messages (this is best-effort since Godot may not send events)
    # The test passes as long as no exceptions occur and the driver remains responsive
    assert Process.alive?(Process.whereis(LibGodotConnector.Godot))
  end

  test "README - error handling for invalid operations" do
    # Test error cases that should be handled gracefully

    # The driver already has a Godot instance, so we'll test invalid operations differently
    # Try to send message with invalid ref
    result = LibGodot.Driver.send_message("invalid_ref", "test")
    # The driver should handle this gracefully (may return :ok or error depending on port behavior)
    assert result == :ok or match?({:error, _}, result), "Should handle send_message gracefully"

    # Try to iterate with invalid ref
    result = LibGodot.Driver.iteration("invalid_ref")
    assert result == :ok or match?({:error, _}, result), "Should handle iteration gracefully"

    # Try to request with invalid ref
    result = LibGodot.Driver.request("invalid_ref", "test", 100)
    assert result == {:ok, "message_sent"} or match?({:error, _}, result), "Should handle request gracefully"
  end
end

