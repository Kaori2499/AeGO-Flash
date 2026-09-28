foreach(name COMPILER SOURCE HEADERS RESOURCES PIPL_TOOL OUTPUT)
    if(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
        message(FATAL_ERROR "BuildPiPL requires ${name}")
    endif()
endforeach()
set(preprocessed "${OUTPUT}.rr")
set(resource_source "${OUTPUT}.rrc")
execute_process(COMMAND "${COMPILER}" /nologo /EP /TC /DMSWindows /D_M_X64
    "/I${HEADERS}" "/I${RESOURCES}" "${SOURCE}"
    OUTPUT_FILE "${preprocessed}" ERROR_VARIABLE errors RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "PiPL preprocessing failed: ${errors}")
endif()
execute_process(COMMAND "${PIPL_TOOL}" "${preprocessed}" "${resource_source}"
    OUTPUT_VARIABLE output_log ERROR_VARIABLE errors RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Adobe PiPLtool failed: ${output_log} ${errors}")
endif()
execute_process(COMMAND "${COMPILER}" /nologo /EP /TC /DMSWindows "${resource_source}"
    OUTPUT_FILE "${OUTPUT}" ERROR_VARIABLE errors RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "PiPL resource conversion failed: ${errors}")
endif()
