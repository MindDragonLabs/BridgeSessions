# cli_quick_connect_self_refused — `bridgesessions <own-hostname>` must refuse
# with the self-connect message (regression test for the misleading
# "untrusted first contact" path when targeting yourself).
cmake_host_system_information(RESULT HOSTNAME QUERY HOSTNAME)
execute_process(
    COMMAND "${BS_BINARY}" --config-dir "${TEST_DIR}" "${HOSTNAME}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err)
set(combined "${out}${err}")
if(rc EQUAL 0)
    message(FATAL_ERROR "self-connect unexpectedly succeeded")
endif()
if(NOT combined MATCHES "Cannot connect to yourself")
    message(FATAL_ERROR "missing self-connect refusal; got: ${combined}")
endif()
