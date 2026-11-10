cmake_minimum_required(VERSION 3.21)

project(magic_portal VERSION 0.2.0 LANGUAGES CXX)

# Apple's ar and ranlib otherwise record wall-clock timestamps, even when the object files
# are identical. Wrap both archive creation and indexing so ordinary builds are reproducible,
# including when the library is consumed through add_subdirectory(). Keep the toolchain's rules.
if(APPLE)
    foreach(_mp_rule IN ITEMS CMAKE_CXX_ARCHIVE_CREATE CMAKE_CXX_ARCHIVE_APPEND CMAKE_CXX_ARCHIVE_FINISH)
        set(_mp_commands "")
        foreach(_mp_command IN LISTS ${_mp_rule})
            list(APPEND _mp_commands "\"${CMAKE_COMMAND}\" -E env ZERO_AR_DATE=1 ${_mp_command}")
        endforeach()
        set(${_mp_rule} "${_mp_commands}")
    endforeach()
endif()

# ---------------------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------------------
option(MAGIC_PORTAL_BUILD_TESTS "Build and register the test suite" ON)
option(MAGIC_PORTAL_BUILD_TOOLS "Build the demo, benchmark and evaluation tools" ON)
option(MAGIC_PORTAL_BUILD_SHARED "Build a shared library that exports only the C interface" OFF)
option(MAGIC_PORTAL_WITH_TFLITE "Build the TensorFlow Lite C API segmenter" OFF)
option(MAGIC_PORTAL_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(MAGIC_PORTAL_SANITIZE "" CACHE STRING "Sanitizers, e.g. 'address;undefined' or 'thread'")
option(MAGIC_PORTAL_INSTALL "Generate install rules" ${PROJECT_IS_TOP_LEVEL})

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES AND PROJECT_IS_TOP_LEVEL)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The version lives in the public C header; keep the CMake project in step with it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/magic_portal/magic_portal.h" _mp_version_lines
     REGEX "^#define MP_VERSION_(MAJOR|MINOR|PATCH) ")
set(_mp_header_version "")
foreach(_line IN LISTS _mp_version_lines)
    string(REGEX REPLACE "^#define MP_VERSION_[A-Z]+ +([0-9]+).*$" "\\1" _number "${_line}")
    list(APPEND _mp_header_version "${_number}")
endforeach()
string(REPLACE ";" "." _mp_header_version "${_mp_header_version}")
if(NOT _mp_header_version STREQUAL PROJECT_VERSION)
    message(FATAL_ERROR
        "Version mismatch: CMake project is ${PROJECT_VERSION} but magic_portal.h says ${_mp_header_version}")
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------------------
set(MP_CORE_SOURCES
    src/frame.cpp
    src/mask_refiner.cpp
    src/portal_geometry.cpp
    src/portal_compositor.cpp
    src/portal_timeline.cpp
    src/portal_controller.cpp
    src/portal_shader.cpp
    src/segmenter_io.cpp
    src/engine.cpp
    src/portal_runner.cpp
    src/pipeline.cpp
    src/c_api.cpp
)

if(MAGIC_PORTAL_WITH_TFLITE)
    if(NOT DEFINED TFLITE_INCLUDE_DIR OR NOT DEFINED TFLITE_LIBRARY)
        message(FATAL_ERROR
            "Set TFLITE_INCLUDE_DIR and TFLITE_LIBRARY when MAGIC_PORTAL_WITH_TFLITE=ON")
    endif()
    set(MP_TFLITE_SOURCE src/tflite_person_segmenter.cpp)
else()
    set(MP_TFLITE_SOURCE src/tflite_stub.cpp)
endif()

# ---------------------------------------------------------------------------------------
# Common build settings (private: never imposed on consumers)
# ---------------------------------------------------------------------------------------
set(MP_COMPILE_OPTIONS "")
set(MP_LINK_OPTIONS "")
if(MSVC)
    list(APPEND MP_COMPILE_OPTIONS /W4 /permissive-)
    if(MAGIC_PORTAL_WARNINGS_AS_ERRORS)
        list(APPEND MP_COMPILE_OPTIONS /WX)
    endif()
else()
    list(APPEND MP_COMPILE_OPTIONS
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        # Reproducibility: forbid fused multiply-add contraction so results match across
        # compilers and CPUs instead of depending on each toolchain's default.
        -ffp-contract=off)
    if(MAGIC_PORTAL_WARNINGS_AS_ERRORS)
        list(APPEND MP_COMPILE_OPTIONS -Werror)
    endif()
endif()
if(MAGIC_PORTAL_SANITIZE)
    string(REPLACE ";" "," _mp_sanitize_flag "${MAGIC_PORTAL_SANITIZE}")
    list(APPEND MP_COMPILE_OPTIONS -fsanitize=${_mp_sanitize_flag} -fno-omit-frame-pointer)
    list(APPEND MP_LINK_OPTIONS -fsanitize=${_mp_sanitize_flag})
    if("undefined" IN_LIST MAGIC_PORTAL_SANITIZE)
        # Recovering UBSan diagnostics can leave a test's exit code at zero. Fail the process
        # at the first diagnostic so CTest cannot report undefined behaviour as a passing test.
        set(_mp_no_recover_flag -fno-sanitize-recover=undefined)
        list(APPEND MP_COMPILE_OPTIONS ${_mp_no_recover_flag})
    endif()
endif()

# Consumed by tests and tools only. The library applies the same settings directly so that
# nothing here leaks into the exported package.
add_library(magic_portal_build_settings INTERFACE)
target_compile_options(magic_portal_build_settings INTERFACE ${MP_COMPILE_OPTIONS})
target_link_options(magic_portal_build_settings INTERFACE ${MP_LINK_OPTIONS})
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------------------
# Library
# ---------------------------------------------------------------------------------------
function(mp_configure_library target)
    target_sources(${target} PRIVATE ${MP_CORE_SOURCES} ${MP_TFLITE_SOURCE})
    target_include_directories(${target} PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    target_compile_features(${target} PUBLIC cxx_std_17)
    target_compile_options(${target} PRIVATE ${MP_COMPILE_OPTIONS})
    target_link_options(${target} PRIVATE ${MP_LINK_OPTIONS})
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(UNIX AND NOT APPLE AND NOT ANDROID)
        # The effect code calls sinf/cosf/sqrtf. Declaring libm lets a plain C link of the
        # static library (gcc driver, no C++ runtime) resolve them; bionic and macOS have it
        # in libc and libSystem.
        target_link_libraries(${target} PRIVATE m)
    endif()
    if(MAGIC_PORTAL_WITH_TFLITE)
        target_include_directories(${target} PRIVATE "${TFLITE_INCLUDE_DIR}")
        target_link_libraries(${target} PRIVATE "${TFLITE_LIBRARY}")
        target_compile_definitions(${target} PRIVATE MAGIC_PORTAL_WITH_TFLITE=1)
    endif()
endfunction()

if(MAGIC_PORTAL_BUILD_SHARED)
    add_library(magic_portal SHARED)
    target_compile_definitions(magic_portal PRIVATE MAGIC_PORTAL_BUILDING_SHARED PUBLIC MAGIC_PORTAL_SHARED)
    if(UNIX AND NOT APPLE)
        target_link_options(magic_portal PRIVATE
            "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/cmake/magic_portal.map")
        set_target_properties(magic_portal PROPERTIES
            LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/magic_portal.map")
    endif()
    set_target_properties(magic_portal PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR})
else()
    add_library(magic_portal STATIC)
    set_target_properties(magic_portal PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
mp_configure_library(magic_portal)
add_library(magic_portal::magic_portal ALIAS magic_portal)

# Tests and tools exercise C++ internals. A shared build exports only the C ABI, so give
# them a static twin of the same sources.
if(MAGIC_PORTAL_BUILD_SHARED AND (MAGIC_PORTAL_BUILD_TESTS OR MAGIC_PORTAL_BUILD_TOOLS))
    add_library(magic_portal_static_twin STATIC EXCLUDE_FROM_ALL)
    mp_configure_library(magic_portal_static_twin)
    set(MP_INTERNAL_LIB magic_portal_static_twin)
else()
    set(MP_INTERNAL_LIB magic_portal)
endif()

# ---------------------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------------------
if(MAGIC_PORTAL_INSTALL)
    install(TARGETS magic_portal EXPORT magic_portal-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} FILES_MATCHING PATTERN "*.h")
    install(EXPORT magic_portal-targets NAMESPACE magic_portal:: FILE magic_portal-targets.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/magic_portal)
    configure_package_config_file(cmake/magic_portal-config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/magic_portal-config.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/magic_portal)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/magic_portal-config-version.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/magic_portal-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/magic_portal-config-version.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/magic_portal)
    install(DIRECTORY adapters/ DESTINATION ${CMAKE_INSTALL_DATADIR}/magic_portal/adapters)
    install(FILES LICENSE README.md CHANGELOG.md THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/magic_portal OPTIONAL)
endif()

# ---------------------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------------------
if(MAGIC_PORTAL_BUILD_TOOLS)
    add_executable(magic_portal_demo tools/render_demo.cpp)
    target_link_libraries(magic_portal_demo PRIVATE ${MP_INTERNAL_LIB} magic_portal_build_settings)
    add_executable(magic_portal_bench tools/benchmark.cpp)
    target_include_directories(magic_portal_bench PRIVATE tests tools)
    target_link_libraries(magic_portal_bench PRIVATE ${MP_INTERNAL_LIB} magic_portal_build_settings)
    add_executable(magic_portal_eval tools/eval_masks.cpp)
    target_include_directories(magic_portal_eval PRIVATE tests tools)
    target_link_libraries(magic_portal_eval PRIVATE ${MP_INTERNAL_LIB} magic_portal_build_settings)

    # Writes the fixtures for tools/shader_parity/index.html (GPU against CPU reference). The
    # real Viro adapter is compiled against the stub ViroCore headers to capture its uniforms.
    add_executable(magic_portal_shader_parity_gen
        tools/shader_parity/shader_parity_gen.cpp adapters/viro/ViroMagicPortalMaterial.cpp)
    target_include_directories(magic_portal_shader_parity_gen PRIVATE
        tests tests/viro_stubs adapters/viro)
    target_link_libraries(magic_portal_shader_parity_gen PRIVATE ${MP_INTERNAL_LIB} magic_portal_build_settings)
endif()

# ---------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------
if(MAGIC_PORTAL_BUILD_TESTS)
    enable_testing()
    enable_language(C)
    set(CMAKE_C_STANDARD 11)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(CMAKE_C_EXTENSIONS OFF)

    add_library(magic_portal_test_support INTERFACE)
    target_include_directories(magic_portal_test_support INTERFACE tests tools)
    target_link_libraries(magic_portal_test_support INTERFACE magic_portal_build_settings)

    add_executable(magic_portal_legacy_tests tests/test_main.cpp)
    target_link_libraries(magic_portal_legacy_tests PRIVATE ${MP_INTERNAL_LIB} magic_portal_test_support)
    add_test(NAME legacy COMMAND magic_portal_legacy_tests)

    add_executable(magic_portal_unit_tests
        tests/unit_main.cpp tests/test_core.cpp tests/test_shader.cpp tests/test_segmenter_io.cpp
        tests/test_engine.cpp tests/test_runner.cpp tests/test_eval_metrics.cpp tests/test_c_api.cpp tests/test_eval_cli.cpp)
    target_link_libraries(magic_portal_unit_tests PRIVATE ${MP_INTERNAL_LIB} magic_portal_test_support)
    add_test(NAME unit COMMAND magic_portal_unit_tests)

    # Golden regression files were recorded from the original implementation before any
    # refactoring; this test is what proves later changes did not alter behaviour.
    add_executable(magic_portal_golden_tests tests/golden_test.cpp)
    target_link_libraries(magic_portal_golden_tests PRIVATE ${MP_INTERNAL_LIB} magic_portal_test_support)
    add_test(NAME golden COMMAND magic_portal_golden_tests --check "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
    add_custom_target(update_goldens
        COMMAND magic_portal_golden_tests --update "${CMAKE_CURRENT_SOURCE_DIR}/tests/golden"
        COMMENT "Re-recording golden files (review the diff before committing!)")

    # The public C header must be valid C11 and must work without any C++ in the client.
    add_executable(magic_portal_c_api_smoke tests/c_api_smoke.c)
    target_link_libraries(magic_portal_c_api_smoke PRIVATE magic_portal)
    if(NOT MSVC)
        target_compile_options(magic_portal_c_api_smoke PRIVATE -Wall -Wextra -Wpedantic)
        if(MAGIC_PORTAL_WARNINGS_AS_ERRORS)
            target_compile_options(magic_portal_c_api_smoke PRIVATE -Werror)
        endif()
        if(MAGIC_PORTAL_SANITIZE)
            target_compile_options(magic_portal_c_api_smoke PRIVATE
                -fsanitize=${_mp_sanitize_flag} ${_mp_no_recover_flag})
            target_link_options(magic_portal_c_api_smoke PRIVATE -fsanitize=${_mp_sanitize_flag})
        endif()
    endif()
    set_target_properties(magic_portal_c_api_smoke PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME c_api_smoke COMMAND magic_portal_c_api_smoke)

    # Every public header must compile on its own (no hidden include-order dependencies).
    file(GLOB _mp_public_headers RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/include"
         "${CMAKE_CURRENT_SOURCE_DIR}/include/magic_portal/*.h")
    set(_mp_header_check_sources "")
    foreach(_header IN LISTS _mp_public_headers)
        string(REPLACE "/" "_" _stem "${_header}")
        string(MAKE_C_IDENTIFIER "${_stem}" _id)
        set(_file "${CMAKE_CURRENT_BINARY_DIR}/header_check/${_stem}.cpp")
        file(GENERATE OUTPUT "${_file}" CONTENT "#include \"${_header}\"\nint header_check_${_id};\n")
        list(APPEND _mp_header_check_sources "${_file}")
    endforeach()
    add_library(magic_portal_header_check OBJECT ${_mp_header_check_sources})
    target_link_libraries(magic_portal_header_check PRIVATE magic_portal magic_portal_build_settings)

    # TFLite glue against a test double of the C API (the real runtime is not required).
    add_executable(magic_portal_tflite_glue_tests
        tests/unit_main.cpp tests/test_tflite_glue.cpp tests/fake_tflite/fake_tflite.cpp
        ${MP_CORE_SOURCES} src/tflite_person_segmenter.cpp)
    target_include_directories(magic_portal_tflite_glue_tests PRIVATE
        include tests tests/fake_tflite)
    target_compile_definitions(magic_portal_tflite_glue_tests PRIVATE MAGIC_PORTAL_WITH_TFLITE=1)
    target_compile_features(magic_portal_tflite_glue_tests PRIVATE cxx_std_17)
    target_link_libraries(magic_portal_tflite_glue_tests PRIVATE magic_portal_build_settings Threads::Threads)
    add_test(NAME tflite_glue COMMAND magic_portal_tflite_glue_tests)

    # The Viro adapter compiled against stub ViroCore/GLES headers. This checks the adapter's
    # own logic; it does not validate signatures against the real ViroCore.
    add_executable(magic_portal_viro_adapter_tests
        tests/unit_main.cpp tests/test_viro_adapter.cpp
        adapters/viro/ViroMagicPortalMaterial.cpp
        adapters/viro/android/ViroMagicPortalFrameListener.cpp)
    target_include_directories(magic_portal_viro_adapter_tests PRIVATE
        tests tests/viro_stubs adapters/viro adapters/viro/android)
    target_link_libraries(magic_portal_viro_adapter_tests PRIVATE
        ${MP_INTERNAL_LIB} magic_portal_test_support)
    add_test(NAME viro_adapter_stubs COMMAND magic_portal_viro_adapter_tests)

    # Install the package into a scratch prefix and build a separate project against it with
    # find_package(), exactly as an integrator would.
    if(MAGIC_PORTAL_INSTALL)
        add_test(NAME consumer_package
            COMMAND ${CMAKE_COMMAND}
                -DMP_BUILD_DIR=${CMAKE_BINARY_DIR}
                -DMP_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
                -DMP_WORK_DIR=${CMAKE_BINARY_DIR}/consumer_check
                -DMP_CONFIG=$<CONFIG>
                -DMP_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                -DMP_C_COMPILER=${CMAKE_C_COMPILER}
                -DMP_SANITIZE=${_mp_sanitize_flag}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_consumer_test.cmake)
    endif()
endif()
