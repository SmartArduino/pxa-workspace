cmake_minimum_required(VERSION 3.16)

# Query the same metadata used by firmware, without configuring ESP-IDF.
# Callers use --log-level=ERROR so metadata status messages do not enter stdout.
if(NOT DEFINED PXA_BOARD_METADATA OR NOT EXISTS "${PXA_BOARD_METADATA}")
    message(FATAL_ERROR "Board metadata is unavailable: ${PXA_BOARD_METADATA}")
endif()
unset(PXA_BOARD_TARGET)
include("${PXA_BOARD_METADATA}")
if(NOT DEFINED PXA_BOARD_TARGET OR NOT PXA_BOARD_TARGET MATCHES "^[A-Za-z0-9_-]+$")
    message(FATAL_ERROR "Declare a single PXA_BOARD_TARGET in ${PXA_BOARD_METADATA}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${PXA_BOARD_TARGET}"
                RESULT_VARIABLE query_status)
if(NOT query_status EQUAL 0)
    message(FATAL_ERROR "Unable to print board target")
endif()
