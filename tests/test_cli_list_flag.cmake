# cli_list_flag — `bridgesessions --list` without a PEER must exit 0 with the
# daemon's SESSIONS table (or a clear daemon-unreachable error, rc=1).
# Regression test for the `bs <peer> --list` / `bs --list` flag.
execute_process(
    COMMAND "${BS_BINARY}" --config-dir "${TEST_DIR}" --list
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(combined "${out}${err}")
# The isolated config-dir has no daemon; expect the graceful rc=1 path.
if(rc EQUAL 0)
    # If a daemon answered, output must look like the SESSIONS table.
    if(NOT combined MATCHES "SCOPE")
        message(FATAL_ERROR "--list rc=0 but output lacks a session table: ${combined}")
    endif()
    message(STATUS "cli_list_flag: local daemon answered (unexpected in isolation, accepted)")
elseif(rc EQUAL 1)
    if(NOT combined MATCHES "daemon not reachable")
        message(FATAL_ERROR "rc=1 but missing daemon-unreachable message: ${combined}")
    endif()
else()
    message(FATAL_ERROR "--list exited ${rc}; expected 0 (table) or 1 (no daemon): ${combined}")
endif()

# `bs --list somepeer` with no such peer configured must be a clean refusal (rc=2),
# never an attach attempt. Unknown names resolve through `ssh -G` (which fabricates
# defaults for ANY name), so the observed refusal is the untrusted-first-contact one.
execute_process(
    COMMAND "${BS_BINARY}" --config-dir "${TEST_DIR}" "bs-no-such-peer-xyz" --list
    RESULT_VARIABLE rc2
    OUTPUT_VARIABLE out2
    ERROR_VARIABLE err2)
set(combined2 "${out2}${err2}")
if(NOT rc2 EQUAL 2)
    message(FATAL_ERROR "unknown-peer --list exited ${rc2}; expected 2: ${combined2}")
endif()
if(NOT combined2 MATCHES "Refusing untrusted first contact" AND
   NOT combined2 MATCHES "not a configured BridgeSessions peer")
    message(FATAL_ERROR "missing unknown-peer refusal: ${combined2}")
endif()
