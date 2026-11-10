# Installs the build into a scratch prefix, then configures, builds and runs a separate
# project that consumes it through find_package(). Invoked by CTest.
foreach(var MP_BUILD_DIR MP_CONSUMER_SOURCE MP_WORK_DIR)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${MP_WORK_DIR}")
file(MAKE_DIRECTORY "${MP_WORK_DIR}")
set(prefix "${MP_WORK_DIR}/prefix")

set(install_cmd ${CMAKE_COMMAND} --install "${MP_BUILD_DIR}" --prefix "${prefix}")
if(MP_CONFIG)
    list(APPEND install_cmd --config "${MP_CONFIG}")
endif()
execute_process(COMMAND ${install_cmd} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install failed:\n${out}\n${err}")
endif()

set(configure_cmd ${CMAKE_COMMAND} -S "${MP_CONSUMER_SOURCE}" -B "${MP_WORK_DIR}/build"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_CXX_COMPILER=${MP_CXX_COMPILER}" "-DCMAKE_C_COMPILER=${MP_C_COMPILER}")
# A static library built with sanitizers needs the same runtime at the consumer's link step.
if(MP_SANITIZE)
    list(APPEND configure_cmd "-DCMAKE_C_FLAGS=-fsanitize=${MP_SANITIZE}"
        "-DCMAKE_CXX_FLAGS=-fsanitize=${MP_SANITIZE}"
        "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=${MP_SANITIZE}")
endif()
execute_process(COMMAND ${configure_cmd} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "consumer configure failed:\n${out}\n${err}")
endif()

execute_process(COMMAND ${CMAKE_COMMAND} --build "${MP_WORK_DIR}/build"
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "consumer build failed:\n${out}\n${err}")
endif()

foreach(exe consumer_cpp consumer_c)
    if(NOT EXISTS "${MP_WORK_DIR}/build/${exe}")
        message(STATUS "${exe}: not built for this library type, skipped")
        continue()
    endif()
    execute_process(COMMAND "${MP_WORK_DIR}/build/${exe}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${exe} failed (${rc}):\n${out}\n${err}")
    endif()
    message(STATUS "${exe}: ${out}")
endforeach()
