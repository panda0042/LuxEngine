cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED TEST_ROOT OR NOT DEFINED TEST_GENERATOR)
    message(FATAL_ERROR "TEST_ROOT and TEST_GENERATOR are required.")
endif()
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
file(TO_CMAKE_PATH "${CMAKE_CURRENT_LIST_DIR}/../ImportedConfigurations.cmake" mappings)
set(fixture [=[
cmake_minimum_required(VERSION 3.24)
project(ImportedConfigurationRegression NONE)
include("@mappings@")
add_executable(Qt6::moc IMPORTED)
set_target_properties(Qt6::moc PROPERTIES IMPORTED_LOCATION "${CMAKE_COMMAND}")
add_library(SDL3::SDL3-shared SHARED IMPORTED)
set_target_properties(SDL3::SDL3-shared PROPERTIES
    IMPORTED_CONFIGURATIONS Release
    IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/sdl-release.dll")
add_library(Jolt::Jolt STATIC IMPORTED)
set_target_properties(Jolt::Jolt PROPERTIES
    IMPORTED_CONFIGURATIONS Release
    IMPORTED_LOCATION_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/jolt-release.lib"
    INTERFACE_COMPILE_DEFINITIONS "$<$<CONFIG:Debug>:JOLT_DEBUG>")
if(NOT RELEASE_ONLY)
    set_property(TARGET Jolt::Jolt APPEND PROPERTY IMPORTED_CONFIGURATIONS Debug)
    set_property(TARGET Jolt::Jolt PROPERTY IMPORTED_LOCATION_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/jolt-debug.lib")
endif()
lux_configure_jolt_imports()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/selected-$<CONFIG>.txt" CONTENT
    "$<TARGET_FILE:Qt6::moc>\n$<TARGET_FILE:SDL3::SDL3-shared>\n$<TARGET_FILE:Jolt::Jolt>\n$<TARGET_GENEX_EVAL:Jolt::Jolt,$<TARGET_PROPERTY:Jolt::Jolt,INTERFACE_COMPILE_DEFINITIONS>>\n")
]=])
string(CONFIGURE "${fixture}" fixture @ONLY)
file(WRITE "${TEST_ROOT}/source/CMakeLists.txt" "${fixture}")

foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
    set(binary "${TEST_ROOT}/${configuration}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}/source" -B "${binary}"
        -G "${TEST_GENERATOR}" "-DCMAKE_BUILD_TYPE=${configuration}"
        "-DCMAKE_CONFIGURATION_TYPES=${configuration}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${configuration} imported-target generation failed:\n${output}\n${error}")
    endif()
    file(READ "${binary}/selected-${configuration}.txt" selected)
    if(NOT selected MATCHES "sdl-release.dll")
        message(FATAL_ERROR "${configuration} did not preserve SDL's Release-only import.")
    endif()
    if(configuration STREQUAL "Debug")
        if(NOT selected MATCHES "jolt-debug.lib\nJOLT_DEBUG\n")
            message(FATAL_ERROR "Debug must select the Debug Jolt archive and definitions.")
        endif()
    elseif(NOT selected MATCHES "jolt-release.lib\n\n")
        message(FATAL_ERROR "${configuration} must select the Release Jolt archive and definitions.")
    endif()
endforeach()

foreach(configuration Debug Release)
    set(binary "${TEST_ROOT}/release-only-${configuration}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${TEST_ROOT}/source" -B "${binary}"
        -G "${TEST_GENERATOR}" "-DCMAKE_BUILD_TYPE=${configuration}"
        "-DCMAKE_CONFIGURATION_TYPES=${configuration}" -DRELEASE_ONLY=ON
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(configuration STREQUAL "Debug" AND result EQUAL 0)
        message(FATAL_ERROR "A missing Debug Jolt archive must not fall back to Release.")
    elseif(configuration STREQUAL "Release" AND NOT result EQUAL 0)
        message(FATAL_ERROR "A Release-only Jolt install must support Release:\n${output}\n${error}")
    endif()
endforeach()
message(STATUS "Qt unconfigured imports, SDL Release imports and strict Jolt configurations passed.")
