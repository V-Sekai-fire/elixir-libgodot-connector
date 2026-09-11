#!/usr/bin/env bash
# libgodot_host smoke tests, paired with negative controls per CLAUDE.md rule 2:
# "A check that passes on known-broken input is decoration — it certifies the
# defect. Every gate ships with a negative control asserting the broken input
# fails." Each positive assertion below is followed by a control that would
# pass if the assertion were a no-op.
#
# Usage:
#   env HOST=/path/to/libgodot_host LIBGODOT=/path/to/libgodot.dylib \
#       PROJECT_DIR=/path/to/dir/containing/main.gd \
#       bash smoke_test.sh
#
# Exits 0 when every positive passes AND every control fails as it should.
set -u

HOST=${HOST:-}
LIBGODOT=${LIBGODOT:-}
PROJECT_DIR=${PROJECT_DIR:-}

if [ -z "$HOST" ] || [ -z "$LIBGODOT" ] || [ -z "$PROJECT_DIR" ]; then
    echo "usage: env HOST=... LIBGODOT=... PROJECT_DIR=... $0" >&2
    exit 64
fi

pass=0
fail=0
failed_names=()

run_case() {
    # run_case NAME EXPECTED_EXIT EXPECTED_STDERR_MATCH -- cmd args...
    local name=$1 want_exit=$2 want_match=$3
    shift 3
    [ "$1" = "--" ] && shift
    local out
    out=$( "$@" 2>&1 )
    local rc=$?
    if [ "$rc" -ne "$want_exit" ]; then
        echo "FAIL [$name]: exit $rc, wanted $want_exit"
        echo "----"
        echo "$out" | tail -5 | sed 's/^/  /'
        echo "----"
        fail=$((fail+1)); failed_names+=("$name"); return
    fi
    if [ -n "$want_match" ] && ! echo "$out" | grep -qE "$want_match"; then
        echo "FAIL [$name]: stderr did not match /$want_match/"
        echo "----"
        echo "$out" | tail -5 | sed 's/^/  /'
        echo "----"
        fail=$((fail+1)); failed_names+=("$name"); return
    fi
    echo "PASS [$name]"
    pass=$((pass+1))
}

# All --smoke / --no-headless runs cd into PROJECT_DIR first: libgodot
# discovers project.godot from cwd on macOS and would otherwise pick up
# whatever the shell started in (or fail). PROJECT_DIR must contain the
# stub project.godot + main.gd the smoke path exercises.

# Pair 1: --smoke path (headless, the default).
# Positive: a valid --smoke run exits 0, prints "smoke OK", and does NOT
# initialize a rendering device — Metal / Vulkan / OpenGL messages are
# the fingerprint of a windowed init that should not fire in headless.
control_headless_out=$( ( cd "$PROJECT_DIR" && env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3 2>&1 ) )
if echo "$control_headless_out" | grep -qE "smoke OK" \
   && ! echo "$control_headless_out" | grep -qE "Metal [0-9]|Vulkan API [0-9]|OpenGL API [0-9]"; then
    echo "PASS [smoke_ok]"
    pass=$((pass+1))
else
    echo "FAIL [smoke_ok]: missing 'smoke OK' or renderer initialized in headless"
    echo "----"
    echo "$control_headless_out" | tail -6 | sed 's/^/  /'
    echo "----"
    fail=$((fail+1)); failed_names+=("smoke_ok")
fi

# Control: a --smoke run pointed at a libgodot that does not exist must
# fail (proves the "smoke OK" assertion above is not a no-op that would
# also pass when godot could not load at all). LIBGODOT_PATH is the
# earliest failure point — dlopen returns null and the host exits 2 via
# die("dlopen"). Using a bogus --script does not qualify as a control
# because godot's -s falls back to the project.godot in cwd.
run_case "smoke_bad_libgodot" 2 "dlopen" -- \
    bash -c "cd '$PROJECT_DIR' && env LIBGODOT_PATH=/nonexistent/libgodot.dylib '$HOST' --smoke --script '$PROJECT_DIR/main.gd' --max-iterations 3"

# Pair 2: bus mode without iceoryx2.
# Positive: bus mode with no iceoryx2 loadable exits with weft's own
# non-zero code (run_command_loop returns 1 -> host propagates rc=1)
# AND prints the WEFT_ICEORYX2_PATH diagnostic from weft::load_bus.
# Both signals must be present: an exit code without the diagnostic
# would be indistinguishable from an unrelated failure.
# Runs with WEFT_ICEORYX2_PATH forced to a nonexistent path so the case
# still exercises the load_bus failure fingerprint even when iceoryx2
# IS built and reachable in the ambient env (as it should be after the
# workspace has been provisioned).
run_case "bus_unreachable_iceoryx2" 1 "libiceoryx2_ffi_c|WEFT_ICEORYX2_PATH" -- \
    bash -c "cd '$PROJECT_DIR' && env LIBGODOT_PATH='$LIBGODOT' WEFT_ICEORYX2_PATH=/nonexistent/libiceoryx2_ffi_c.dylib '$HOST' --script '$PROJECT_DIR/main.gd'"

# Control: the same diagnostic text must NOT appear in a --smoke run (that
# path never touches the bus). If it did, the pattern would be matching
# something unrelated and the positive above would be decoration.
control_out=$( ( cd "$PROJECT_DIR" && env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3 2>&1 ) )
if echo "$control_out" | grep -qE "libiceoryx2_ffi_c|WEFT_ICEORYX2_PATH"; then
    echo "FAIL [bus_diag_only_on_bus_path]: smoke path emitted the bus diagnostic"
    fail=$((fail+1)); failed_names+=("bus_diag_only_on_bus_path")
else
    echo "PASS [bus_diag_only_on_bus_path]"
    pass=$((pass+1))
fi

# Pair 3: --no-headless brings up a rendering device.
# Positive: --smoke --no-headless exits 0 AND the platform-specific
# renderer log fires (Metal on macOS, Vulkan on Linux, D3D12/Vulkan on
# Windows). Proves the flag actually reaches godot's Main::setup;
# without it, the flag would be silently ignored and windowed mode would
# look identical to headless.
no_headless_out=$( ( cd "$PROJECT_DIR" && env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --no-headless --script "$PROJECT_DIR/main.gd" --max-iterations 3 2>&1 ) )
if echo "$no_headless_out" | grep -qE "smoke OK" \
   && echo "$no_headless_out" | grep -qE "Metal [0-9]|Vulkan API [0-9]|OpenGL API [0-9]|D3D12"; then
    echo "PASS [no_headless_ok]"
    pass=$((pass+1))
else
    echo "FAIL [no_headless_ok]: missing 'smoke OK' or renderer never initialized"
    echo "----"
    echo "$no_headless_out" | tail -6 | sed 's/^/  /'
    echo "----"
    fail=$((fail+1)); failed_names+=("no_headless_ok")
fi

# Control: --no-headless and the default (headless) run of the same
# command must differ in the renderer signature — otherwise the flag is
# a no-op and the positive above is decoration.
if diff <(echo "$control_headless_out" | grep -E "Metal [0-9]|Vulkan API [0-9]|OpenGL API [0-9]|D3D12" || echo "no-renderer") \
        <(echo "$no_headless_out"      | grep -E "Metal [0-9]|Vulkan API [0-9]|OpenGL API [0-9]|D3D12" || echo "no-renderer") \
        >/dev/null; then
    echo "FAIL [no_headless_differs_from_headless]: renderer signature was identical between --headless and --no-headless"
    fail=$((fail+1)); failed_names+=("no_headless_differs_from_headless")
else
    echo "PASS [no_headless_differs_from_headless]"
    pass=$((pass+1))
fi

# Pair 4: real godot-demo-projects project loads and iterates without a
# modal alert. Skipped when DEMO_DIR is unset — the manifest project
# 4-entities/godot-demo-projects has to be synced first. Use a demo
# whose main scene is compute/data-only (no display dependency) so a
# headless run reaches iteration.
if [ -n "${DEMO_DIR:-}" ] && [ -f "$DEMO_DIR/project.godot" ]; then
    demo_out=$( env "LIBGODOT_PATH=$LIBGODOT" \
        "$HOST" --smoke --project "$DEMO_DIR" --max-iterations 3 2>&1 )
    # Positive: the demo actually boots and iterates to completion.
    # "GodotInstance ptr" is the first log the host emits AFTER create
    # returns non-null (so libgodot got past OS init + Main::setup +
    # instance initialize); "ran N iteration(s)" confirms start() plus
    # the iteration loop completed.
    if echo "$demo_out" | grep -qE "GodotInstance ptr" \
       && echo "$demo_out" | grep -qE "ran [0-9]+ iteration"; then
        echo "PASS [demo_project_iterates]"
        pass=$((pass+1))
    else
        echo "FAIL [demo_project_iterates]: demo did not boot to iteration"
        echo "----"; echo "$demo_out" | tail -8 | sed 's/^/  /'; echo "----"
        fail=$((fail+1)); failed_names+=("demo_project_iterates")
    fi

    # Control: the NSAlert regression fingerprint (os_macos.mm:347) must
    # not appear. Without the entities-godot alert-suppression fix, any
    # Main::setup error would call OS_MacOS::alert (line 347) instead of
    # OS_MacOS_Headless::alert (line 1197) and pop a modal. This asserts
    # that specific line number rather than the presence of "alert" text
    # so that a stderr-printed diagnostic from the headless path stays
    # legal.
    if echo "$demo_out" | grep -qE "os_macos\.mm:347"; then
        echo "FAIL [demo_project_no_modal_alert]: OS_MacOS::alert (line 347) fired — modal-alert regression"
        echo "----"; echo "$demo_out" | tail -6 | sed 's/^/  /'; echo "----"
        fail=$((fail+1)); failed_names+=("demo_project_no_modal_alert")
    else
        echo "PASS [demo_project_no_modal_alert]"
        pass=$((pass+1))
    fi
else
    echo "SKIP [demo_project_iterates]: DEMO_DIR unset or missing project.godot"
    echo "SKIP [demo_project_no_modal_alert]: DEMO_DIR unset or missing project.godot"
fi

# Pair 5: P2P bus wiring, exercised via --bus-dry-run.
# --bus-dry-run opens the lifecycle + P2P services against the real
# iceoryx2 loader path, prints the readiness diagnostics, then exits 0
# deterministically. Lifecycle-controlled — no timeouts, no signals,
# no polling. If iceoryx2 is not reachable the run exits non-zero
# with weft's own WEFT_ICEORYX2_PATH diagnostic (also fine — either
# way the test reveals the state of the bus rather than swallowing it).
#
# Positive: --bus-dry-run exits 0 AND the P2P service names appear in
# stderr AND the "bus dry-run OK" fence log is present.
p2p_out=$( bash -c "cd '$PROJECT_DIR' && env LIBGODOT_PATH='$LIBGODOT' '$HOST' --bus-dry-run --script '$PROJECT_DIR/main.gd' 2>&1" )
p2p_rc=$?
if [ "$p2p_rc" -eq 0 ] \
   && echo "$p2p_out" | grep -qE "libgodot_host/p2p" \
   && echo "$p2p_out" | grep -qE "bus dry-run OK"; then
    echo "PASS [p2p_diag_present]"
    pass=$((pass+1))
else
    echo "FAIL [p2p_diag_present]: exit $p2p_rc, missing P2P service name or dry-run OK line"
    echo "----"; echo "$p2p_out" | tail -8 | sed 's/^/  /'; echo "----"
    fail=$((fail+1)); failed_names+=("p2p_diag_present")
fi

# Control: the P2P diagnostic must NOT appear in --smoke output. If
# open_p2p accidentally ran outside bus mode, the positive would pass
# on a code path that never wraps godot in the host loop.
control_p2p_out=$( ( cd "$PROJECT_DIR" && env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3 2>&1 ) )
if echo "$control_p2p_out" | grep -qE "P2P bus (ready|not ready)|libgodot_host/p2p"; then
    echo "FAIL [p2p_diag_only_on_bus_path]: smoke path emitted the P2P diagnostic"
    fail=$((fail+1)); failed_names+=("p2p_diag_only_on_bus_path")
else
    echo "PASS [p2p_diag_only_on_bus_path]"
    pass=$((pass+1))
fi

echo
echo "== $pass passed, $fail failed =="
if [ "$fail" -ne 0 ]; then
    # Bash array expansion under `set -u` can trip when the array is empty
    # (older bashes), so guard the printf explicitly.
    if [ "${#failed_names[@]}" -gt 0 ]; then
        printf '  failed: %s\n' "${failed_names[@]}"
    fi
    exit 1
fi
exit 0
