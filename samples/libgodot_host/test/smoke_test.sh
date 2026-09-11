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

# Pair 1: --smoke path.
# Positive: a valid --smoke run exits 0 and prints "smoke OK".
run_case "smoke_ok" 0 "smoke OK" -- \
    env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3

# Control: a --smoke run pointed at a libgodot that does not exist must
# fail (proves the "smoke OK" assertion above is not a no-op that would
# also pass when godot could not load at all). LIBGODOT_PATH is the
# earliest failure point — dlopen returns null and the host exits 2 via
# die("dlopen"). Using a bogus --script does not qualify as a control
# because godot's -s falls back to the project.godot in cwd.
run_case "smoke_bad_libgodot" 2 "dlopen" -- \
    env "LIBGODOT_PATH=/nonexistent/libgodot.dylib" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3

# Pair 2: bus mode without iceoryx2.
# Positive: bus mode with no iceoryx2 loadable exits with weft's own
# non-zero code (run_command_loop returns 1 -> host propagates rc=1)
# AND prints the WEFT_ICEORYX2_PATH diagnostic from weft::load_bus.
# Both signals must be present: an exit code without the diagnostic
# would be indistinguishable from an unrelated failure.
run_case "bus_unreachable_iceoryx2" 1 "libiceoryx2_ffi_c|WEFT_ICEORYX2_PATH" -- \
    env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --script "$PROJECT_DIR/main.gd"

# Control: the same diagnostic text must NOT appear in a --smoke run (that
# path never touches the bus). If it did, the pattern would be matching
# something unrelated and the positive above would be decoration.
control_out=$( env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3 2>&1 )
if echo "$control_out" | grep -qE "libiceoryx2_ffi_c|WEFT_ICEORYX2_PATH"; then
    echo "FAIL [bus_diag_only_on_bus_path]: smoke path emitted the bus diagnostic"
    fail=$((fail+1)); failed_names+=("bus_diag_only_on_bus_path")
else
    echo "PASS [bus_diag_only_on_bus_path]"
    pass=$((pass+1))
fi

# Pair 3: P2P bus wiring.
# Positive: the bus-mode diagnostic mentions the P2P service names,
# proving the P2P open path runs (or attempts to run) alongside the
# lifecycle open. Same iceoryx2-unreachable run as pair 2, so the
# check is on the presence of the extra diagnostic line, not on
# success — full end-to-end P2P testing needs iceoryx2 built, which
# is a separate follow-up.
run_case "p2p_diag_present" 1 "P2P bus (ready|not ready)|libgodot_host/p2p" -- \
    env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --script "$PROJECT_DIR/main.gd"

# Control: the P2P diagnostic must NOT appear in --smoke output. If
# open_p2p accidentally ran outside bus mode, the positive would pass
# on a code path that never wraps godot in the host loop.
control_p2p_out=$( env "LIBGODOT_PATH=$LIBGODOT" \
    "$HOST" --smoke --script "$PROJECT_DIR/main.gd" --max-iterations 3 2>&1 )
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
