if(NOT DEFINED BS_BINARY)
    message(FATAL_ERROR "BS_BINARY is required")
endif()
if(NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "TEST_DIR is required")
endif()

file(REMOVE_RECURSE "${TEST_DIR}")
file(MAKE_DIRECTORY "${TEST_DIR}")

execute_process(
    COMMAND "${BS_BINARY}" --config-dir "${TEST_DIR}" keygen
    RESULT_VARIABLE first_result
    OUTPUT_VARIABLE first_stdout
    ERROR_VARIABLE first_stderr)
if(NOT first_result EQUAL 0)
    message(FATAL_ERROR
        "initial keygen failed (${first_result}): ${first_stdout}${first_stderr}")
endif()

set(key_path "${TEST_DIR}/id_ed25519.pem")
if(NOT EXISTS "${key_path}")
    message(FATAL_ERROR "initial keygen did not create ${key_path}")
endif()
file(SHA256 "${key_path}" key_sha_before)

execute_process(
    COMMAND "${BS_BINARY}" --config-dir "${TEST_DIR}" keygen
    RESULT_VARIABLE second_result
    OUTPUT_VARIABLE second_stdout
    ERROR_VARIABLE second_stderr)
file(SHA256 "${key_path}" key_sha_after)

if(second_result EQUAL 0)
    message(FATAL_ERROR "second keygen unexpectedly succeeded")
endif()
if(NOT key_sha_before STREQUAL key_sha_after)
    message(FATAL_ERROR "second keygen changed the existing private identity")
endif()

file(REMOVE_RECURSE "${TEST_DIR}")
