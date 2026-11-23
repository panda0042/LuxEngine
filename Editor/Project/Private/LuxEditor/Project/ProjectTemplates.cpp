#include <LuxEditor/Project/ProjectTemplates.hpp>
#include <LuxUI/Widgets.hpp>

#include <fstream>
#include <string_view>

namespace LuxEditor {
namespace {
constexpr const char* GameCMake = R"LUX(cmake_minimum_required(VERSION 3.24)
project(LuxGame LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(LuxEngine CONFIG REQUIRED)
if(NOT LuxEngine_MANAGED_DIR)
    message(FATAL_ERROR
        "LuxEngine SDK at '${LuxEngine_DIR}' does not define LuxEngine_MANAGED_DIR. "
        "Use a current SDK that includes LuxEngine.ScriptCore.dll.")
endif()
set(_lux_script_core "${LuxEngine_MANAGED_DIR}/LuxEngine.ScriptCore.dll")
if(NOT EXISTS "${_lux_script_core}" OR IS_DIRECTORY "${_lux_script_core}")
    message(FATAL_ERROR "LuxEngine SDK at '${LuxEngine_DIR}' is missing '${_lux_script_core}'.")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/Dependencies.cmake OPTIONAL)
add_executable(LuxGame Source/Main.cpp)
find_program(LUX_GAME_DOTNET dotnet HINTS "$ENV{DOTNET_ROOT}" REQUIRED)
set(_lux_managed_output "${CMAKE_CURRENT_SOURCE_DIR}/.lux/managed")
add_custom_target(LuxGameScripts
    COMMAND "${LUX_GAME_DOTNET}" build "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/GameScripts.csproj"
        --configuration Release --output "${_lux_managed_output}"
        --artifacts-path "${CMAKE_CURRENT_BINARY_DIR}/managed-artifacts"
        "-p:LuxEngineScriptCore=${_lux_script_core}"
        "-p:RestoreConfigFile=${CMAKE_CURRENT_SOURCE_DIR}/Scripts/NuGet.Config"
        -p:NuGetAudit=false
    VERBATIM)
add_dependencies(LuxGame LuxGameScripts)
target_link_libraries(LuxGame PRIVATE LuxEngine::Engine LuxEngine::Runtime)
target_compile_definitions(LuxGame PRIVATE LUX_GAME_TEMPLATE_KIND=@KIND@)
if(MSVC)
    # Sources are UTF-8 (Korean text); MSVC would otherwise read them in the system code page.
    target_compile_options(LuxGame PRIVATE /utf-8)
endif()
if(COMMAND lux_copy_runtime)
    lux_copy_runtime(LuxGame NO_PROJECT_CONTENT)
elseif(COMMAND lux_copy_shaders)
    lux_copy_shaders(LuxGame)
endif()
if(NOT "${LUX_PROJECT_MANIFEST}" STREQUAL "")
    set(_lux_manifest "${LUX_PROJECT_MANIFEST}")
    cmake_path(ABSOLUTE_PATH _lux_manifest BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" NORMALIZE)
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/project.luxproject")
    set(_lux_manifest "${CMAKE_CURRENT_SOURCE_DIR}/project.luxproject")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/project.luxproj")
    set(_lux_manifest "${CMAKE_CURRENT_SOURCE_DIR}/project.luxproj")
else()
    file(GLOB _lux_manifests LIST_DIRECTORIES FALSE
        "${CMAKE_CURRENT_SOURCE_DIR}/*.luxproject" "${CMAKE_CURRENT_SOURCE_DIR}/*.luxproj")
    list(LENGTH _lux_manifests _lux_manifest_count)
    if(NOT _lux_manifest_count EQUAL 1)
        message(FATAL_ERROR "Specify the project manifest with -DLUX_PROJECT_MANIFEST=<path>.")
    endif()
    list(GET _lux_manifests 0 _lux_manifest)
endif()
if(NOT EXISTS "${_lux_manifest}" OR IS_DIRECTORY "${_lux_manifest}")
    message(FATAL_ERROR "Project manifest is not a file: ${_lux_manifest}")
endif()
add_custom_target(LuxGameContent
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DBINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
        "-DPROJECT_MANIFEST=${_lux_manifest}"
        "-DCONTENT_ROOT=$<TARGET_FILE_DIR:LuxGame>/Content"
        "-DMANAGED_ROOT=${_lux_managed_output}"
        "-DMANIFEST=${CMAKE_CURRENT_BINARY_DIR}/lux-content-$<CONFIG>.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/SyncContent.cmake"
    VERBATIM)
add_dependencies(LuxGameContent LuxGameScripts)
add_dependencies(LuxGame LuxGameContent)
if(WIN32)
    get_target_property(_engine_type LuxEngine::Engine TYPE)
    if(_engine_type STREQUAL "SHARED_LIBRARY")
        add_custom_command(TARGET LuxGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:LuxEngine::Engine> $<TARGET_FILE_DIR:LuxGame>)
    endif()
    if(TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET LuxGame POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:LuxGame>)
    endif()

endif()
)LUX";

constexpr auto ImageHeaderScript = R"LUX(if(DEFINED LUX_CONTENT_IMAGE_HEADER_PATH)
    file(READ "${LUX_CONTENT_IMAGE_HEADER_PATH}" header LIMIT 92 HEX)
    message("${header}")
    return()
endif()

)LUX";

constexpr auto DirectImageHeaderRead = R"LUX(    file(READ "${path}" header LIMIT 92 HEX)
)LUX";

constexpr auto RequiredImageCheck =
    R"LUX(function(has_image_reference document owner target depth result)
    set(${result} FALSE PARENT_SCOPE)
    if(depth GREATER 16)
        return()
    endif()
    get_filename_component(directory "${owner}" DIRECTORY)
    string(CONCAT pattern
        [=["(texture|sprite_sheet|base_color_texture|normal_texture|metallic_texture|roughness_texture|occlusion_texture|environment|baseline|suspended)"]=]
        "[ \r\n\t]*:[ \r\n\t]*" [=[("(\\.|[^"\\])*")]=])
    while(document MATCHES "${pattern}")
        set(match "${CMAKE_MATCH_0}")
        set(field "${CMAKE_MATCH_1}")
        set(quoted "${CMAKE_MATCH_2}")
        string(FIND "${document}" "${match}" offset)
        string(LENGTH "${match}" length)
        math(EXPR offset "${offset} + ${length}")
        string(SUBSTRING "${document}" ${offset} -1 document)
        string(JSON value ERROR_VARIABLE parseError GET "[${quoted}]" 0)
        if(NOT parseError STREQUAL "NOTFOUND" OR value STREQUAL "")
            continue()
        endif()
        if(field STREQUAL "baseline" OR field STREQUAL "suspended")
            math(EXPR nextDepth "${depth} + 1")
            has_image_reference("${value}" "${owner}" "${target}" ${nextDepth} referenced)
        else()
            cmake_path(ABSOLUTE_PATH value BASE_DIRECTORY "${directory}" NORMALIZE)
            if(caseInsensitivePaths)
                string(TOLOWER "${value}" value)
            endif()
            set(referenced FALSE)
            if(value STREQUAL target)
                set(referenced TRUE)
            endif()
        endif()
        if(referenced)
            set(${result} TRUE PARENT_SCOPE)
            return()
        endif()
    endwhile()
endfunction()

function(require_readable_referenced_image path)
    cmake_path(NORMAL_PATH path OUTPUT_VARIABLE target)
    if(caseInsensitivePaths)
        string(TOLOWER "${target}" target)
    endif()
    foreach(encodedDocument IN LISTS assets)
        decode_path("${encodedDocument}" document)
        string(TOLOWER "${document}" lowerDocument)
        if(NOT lowerDocument MATCHES "[.](lux|luxprefab|luxanim|luxanimator)$" OR
           lowerDocument MATCHES "(^|/)([.]git|[.]lux)(/|$)|^(build|out|cmake-build[^/]*)(/|$)")
            continue()
        endif()
        set(owner "${sourceRoot}/${document}")
        if(IS_DIRECTORY "${owner}" OR NOT EXISTS "${owner}")
            continue()
        endif()
        file(READ "${owner}" data)
        has_image_reference("${data}" "${owner}" "${target}" 0 referenced)
        if(referenced)
            message(FATAL_ERROR "Could not read referenced content: ${path} (referenced by ${document})")
        endif()
    endforeach()
endfunction()

)LUX";

constexpr auto OptionalImageHeaderRead = R"LUX(    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DLUX_CONTENT_IMAGE_HEADER_PATH=${path}"
            -P "${CMAKE_SCRIPT_MODE_FILE}"
        RESULT_VARIABLE readResult ERROR_VARIABLE header OUTPUT_QUIET TIMEOUT 5)
    if(NOT readResult STREQUAL "0")
        if(EXISTS "${MANIFEST}")
            file(READ "${MANIFEST}" previousContent)
            path_lines("${previousContent}" previousPaths)
            cmake_path(RELATIVE_PATH path BASE_DIRECTORY "${sourceRoot}" OUTPUT_VARIABLE relative)
            encode_paths("${relative}" encoded)
            if(caseInsensitivePaths)
                string(TOLOWER "${encoded}" encoded)
                string(TOLOWER "${previousPaths}" previousPaths)
            endif()
            if(encoded IN_LIST previousPaths)
                message(FATAL_ERROR "Could not read previously copied content: ${path}")
            endif()
        endif()
@CHECK_REFERENCED_IMAGE@        return()
    endif()
    string(STRIP "${header}" header)
)LUX";

constexpr auto ImageContentCheck = R"LUX(function(is_image_content path result)
    set(${result} FALSE PARENT_SCOPE)
    if(NOT EXISTS "${path}")
        return()
    endif()
@IMAGE_HEADER_READ@    if(header MATCHES "^(89504e470d0a1a0a|ffd8ff|474946383[79]61|384250530001000000000000|233f52414449414e43450a|233f524742450a|503[56](09|0a|0b|0c|0d|20))")
        set(${result} TRUE PARENT_SCOPE)
        return()
    endif()
    string(LENGTH "${header}" length)
    if(length GREATER_EQUAL 36)
        string(SUBSTRING "${header}" 28 8 dib)
        if(header MATCHES "^424d" AND dib MATCHES "^(0c|28|38|6c|7c)000000$")
            set(${result} TRUE PARENT_SCOPE)
            return()
        endif()
        if(header MATCHES "^[0-9a-f][0-9a-f](000[23ab]|010[19])")
            string(SUBSTRING "${header}" 24 4 width)
            string(SUBSTRING "${header}" 28 4 height)
            string(SUBSTRING "${header}" 32 2 depth)
            if(NOT width STREQUAL "0000" AND NOT height STREQUAL "0000" AND
               depth MATCHES "^(08|0f|10|18|20)$")
                set(${result} TRUE PARENT_SCOPE)
                return()
            endif()
        endif()
    endif()
    if(length GREATER_EQUAL 184 AND header MATCHES "^5380f634")
        string(SUBSTRING "${header}" 176 8 picture)
        if(picture STREQUAL "50494354")
            set(${result} TRUE PARENT_SCOPE)
        endif()
    endif()
endfunction()

)LUX";

constexpr auto LegacyContentSelection = R"LUX(    if(matchPath MATCHES "^Assets/" OR
       (lowerPath MATCHES "[.](lux|luxprefab|luxanim|luxanimator|obj|fbx|png|jpg|jpeg|bmp|tga|hdr)$" AND
        NOT path MATCHES "(^|/)([.]git|[.]lux)(/|$)" AND
        NOT path MATCHES "^(build|out|cmake-build[^/]*)(/|$)"))
        add_content("${source}" "${encoded}")
    endif()
)LUX";

constexpr auto ContentSelection = R"LUX(    if(matchPath MATCHES "^Assets/")
        add_content("${source}" "${encoded}")
    elseif(NOT lowerPath MATCHES "(^|/)([.]git|[.]lux)(/|$)" AND
           NOT lowerPath MATCHES "^(build|out|cmake-build[^/]*)(/|$)")
        if(lowerPath MATCHES "[.](lux|luxprefab|luxanim|luxanimator|obj|fbx|png|jpg|jpeg|bmp|tga|hdr)$")
            add_content("${source}" "${encoded}")
        else()
            is_image_content("${source}" image)
            if(image)
                add_content("${source}" "${encoded}")
            endif()
        endif()
    endif()
)LUX";

constexpr const char* SyncContent =
    R"LUX(cmake_minimum_required(VERSION 3.24)
function(encode_paths text output)
    string(REPLACE "%" "%25" text "${text}")
    string(REPLACE ";" "%3B" text "${text}")
    string(REPLACE "[" "%5B" text "${text}")
    string(REPLACE "]" "%5D" text "${text}")
    string(REPLACE "\\" "%5C" text "${text}")
    set(${output} "${text}" PARENT_SCOPE)
endfunction()

function(decode_path text output)
    string(REPLACE "%5C" "\\" text "${text}")
    string(REPLACE "%5D" "]" text "${text}")
    string(REPLACE "%5B" "[" text "${text}")
    string(REPLACE "%3B" ";" text "${text}")
    string(REPLACE "%25" "%" text "${text}")
    set(${output} "${text}" PARENT_SCOPE)
endfunction()

function(path_lines text output)
    encode_paths("${text}" encoded)
    string(REPLACE "\r\n" "\n" encoded "${encoded}")
    string(REPLACE "\n" ";" paths "${encoded}")
    list(FILTER paths EXCLUDE REGEX "^$")
    set(${output} "${paths}" PARENT_SCOPE)
endfunction()

function(scan_tree directory output)
    if(NOT IS_DIRECTORY "${directory}")
        set(${output} "" PARENT_SCOPE)
        return()
    endif()
    if(WIN32)
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E env "LUX_CONTENT_SCAN_ROOT=${directory}"
                "$ENV{SystemRoot}/System32/WindowsPowerShell/v1.0/powershell.exe"
                -NoProfile -NonInteractive -Command [=[
                $ErrorActionPreference = 'Stop'
                $output = [IO.StreamWriter]::new(
                    [Console]::OpenStandardOutput(), [Text.UTF8Encoding]::new($false))
                $root = [IO.Path]::GetFullPath($env:LUX_CONTENT_SCAN_ROOT)
                $relativeOffset = $root.TrimEnd('\').Length + 1
                $pending = New-Object 'System.Collections.Generic.Stack[string]'
                $pending.Push($root)
                while ($pending.Count -gt 0) {
                    foreach ($entry in [IO.Directory]::EnumerateFileSystemEntries($pending.Pop())) {
                        $output.WriteLine($entry.Substring($relativeOffset).Replace('\', '/'))
                        $attributes = [IO.File]::GetAttributes($entry)
                        if (($attributes -band [IO.FileAttributes]::Directory) -and
                            -not ($attributes -band [IO.FileAttributes]::ReparsePoint)) {
                            $pending.Push($entry)
                        }
                    }
                }
                $output.Flush()
            ]=]
            OUTPUT_VARIABLE listing ENCODING UTF-8 RESULT_VARIABLE result)
    else()
        execute_process(
            COMMAND sh -c [=[
                line_breaks=$(printf '\r\n.')
                line_breaks=${line_breaks%.}
                exec find -P . -mindepth 1 -name "*[$line_breaks]*" -exec sh -c 'exit 1' sh {} + -o -print
            ]=]
            WORKING_DIRECTORY "${directory}" OUTPUT_VARIABLE listing RESULT_VARIABLE result)
        string(REGEX REPLACE "(^|\n)[.]/" "\\1" listing "${listing}")
    endif()
    if(NOT result STREQUAL "0")
        message(FATAL_ERROR "Could not list content paths in ${directory}. Check access and file names containing line breaks.")
    endif()
    path_lines("${listing}" paths)
    list(SORT paths)
    set(${output} "${paths}" PARENT_SCOPE)
endfunction()

@IMAGE_CHECK@function(add_content source encodedPath)
    decode_path("${encodedPath}" path)
    set(key "${path}")
    if(caseInsensitivePaths)
        string(TOLOWER "${key}" key)
    endif()
    string(SHA256 key "${key}")
    if(DEFINED input_${key})
        set(previous "${input_${key}}")
        if(encodedPath STREQUAL path_${key} AND EXISTS "${source}" AND EXISTS "${previous}")
            file(SHA256 "${source}" sourceHash)
            file(SHA256 "${previous}" previousHash)
            if(sourceHash STREQUAL previousHash)
                return()
            endif()
        endif()
        message(FATAL_ERROR "Conflicting generated content for ${path}:\n  ${previous}\n  ${source}")
    endif()
    list(APPEND inputs "${key}")
    list(APPEND current "${encodedPath}")
    set(inputs "${inputs}" PARENT_SCOPE)
    set(current "${current}" PARENT_SCOPE)
    set(input_${key} "${source}" PARENT_SCOPE)
    set(path_${key} "${encodedPath}" PARENT_SCOPE)
endfunction()

file(LOCK "${MANIFEST}.lock" GUARD PROCESS TIMEOUT 30)
if(IS_SYMLINK "${CONTENT_ROOT}")
    message(FATAL_ERROR "Generated content root is a link: ${CONTENT_ROOT}")
endif()
file(MAKE_DIRECTORY "${CONTENT_ROOT}")
file(REAL_PATH "${CONTENT_ROOT}" root)
set(outputLock "${root}.lux-sync-lock.dir")
if(IS_SYMLINK "${outputLock}" OR IS_SYMLINK "${outputLock}/cmake.lock")
    message(FATAL_ERROR "Generated content lock is a link: ${outputLock}")
endif()
file(LOCK "${outputLock}" DIRECTORY GUARD PROCESS TIMEOUT 30)
set(transaction "${root}.lux-sync.dir")
set(nextManifest "${MANIFEST}.lux-sync.tmp")
if(EXISTS "${transaction}" OR IS_SYMLINK "${transaction}" OR
   EXISTS "${nextManifest}" OR IS_SYMLINK "${nextManifest}")
    message(FATAL_ERROR
        "Unfinished content synchronization. Check recovery plans and backups in ${transaction} and ${nextManifest}")
endif()
set(caseInsensitivePaths FALSE)
if(EXISTS "${outputLock}/CMAKE.LOCK")
    set(caseInsensitivePaths TRUE)
endif()

file(REAL_PATH "${SOURCE_ROOT}" sourceRoot)
set(outputRoot "${root}")
set(syncLock "${outputLock}")
set(syncTransaction "${transaction}")
if(caseInsensitivePaths)
    foreach(directory IN ITEMS outputRoot syncLock syncTransaction)
        string(TOLOWER "${${directory}}" ${directory})
    endforeach()
endif()
scan_tree("${sourceRoot}" assets)
scan_tree("${MANAGED_ROOT}" scripts)
add_content("${PROJECT_MANIFEST}" project.luxproject)
foreach(encoded IN LISTS assets)
    decode_path("${encoded}" path)
    set(source "${sourceRoot}/${path}")
    if(IS_DIRECTORY "${source}")
        continue()
    endif()
    set(candidateSource "${source}")
    if(caseInsensitivePaths)
        string(TOLOWER "${candidateSource}" candidateSource)
    endif()
    cmake_path(IS_PREFIX outputRoot "${candidateSource}" NORMALIZE generatedOutput)
    cmake_path(IS_PREFIX syncLock "${candidateSource}" NORMALIZE generatedLock)
    cmake_path(IS_PREFIX syncTransaction "${candidateSource}" NORMALIZE generatedTransaction)
    if(generatedOutput OR generatedLock OR generatedTransaction)
        continue()
    endif()
    if(DEFINED BINARY_ROOT AND NOT BINARY_ROOT STREQUAL SOURCE_ROOT)
        cmake_path(IS_PREFIX BINARY_ROOT "${source}" NORMALIZE generated)
        if(generated)
            continue()
        endif()
    endif()
    set(matchPath "${path}")
    if(WIN32 OR APPLE)
        string(REGEX REPLACE "^[aA][sS][sS][eE][tT][sS]/" "Assets/" matchPath "${matchPath}")
    endif()
    string(TOLOWER "${path}" lowerPath)
@CONTENT_SELECTION@endforeach()
foreach(encoded IN LISTS scripts)
    decode_path("${encoded}" path)
    if(NOT IS_DIRECTORY "${MANAGED_ROOT}/${path}")
        add_content("${MANAGED_ROOT}/${path}" "Scripts/${encoded}")
    endif()
endforeach()

function(content_path relative output)
    if(relative STREQUAL "" OR relative STREQUAL "." OR IS_ABSOLUTE "${relative}" OR
       relative MATCHES "(^|/)[.][.](/|$)" OR relative MATCHES "[:\\\\]")
        message(FATAL_ERROR "Invalid generated content path: ${relative}")
    endif()
    set(candidate "${root}/${relative}")
    set(existing "${candidate}")
    while(NOT EXISTS "${existing}" AND NOT IS_SYMLINK "${existing}")
        cmake_path(GET existing PARENT_PATH existing)
    endwhile()
    set(ancestor "${existing}")
    while(NOT ancestor STREQUAL root)
        if(IS_SYMLINK "${ancestor}")
            message(FATAL_ERROR "Generated content path crosses a link: ${relative}")
        endif()
        cmake_path(GET ancestor PARENT_PATH ancestor)
    endwhile()
    file(REAL_PATH "${existing}" resolved)
    cmake_path(IS_PREFIX root "${resolved}" NORMALIZE inside)
    if(WIN32)
        string(TOLOWER "${resolved}" resolved)
        string(TOLOWER "${existing}" existing)
    endif()
    if(NOT inside OR NOT resolved STREQUAL existing)
        message(FATAL_ERROR "Generated content path crosses a link: ${relative}")
    endif()
    set(${output} "${candidate}" PARENT_SCOPE)
endfunction()

set(staleFiles)
set(staleDirectories)
set(previousPaths)
if(EXISTS "${MANIFEST}")
    file(READ "${MANIFEST}" previousContents)
    path_lines("${previousContents}" previousPaths)
    foreach(encoded IN LISTS previousPaths)
        decode_path("${encoded}" path)
        if(encoded IN_LIST current OR IS_ABSOLUTE "${path}" OR
           path MATCHES "(^|/)[.][.](/|$)" OR path MATCHES "[:\\\\]")
            continue()
        endif()
        content_path("${path}" candidate)
        list(APPEND staleFiles "${encoded}")
        cmake_path(GET path PARENT_PATH directory)
        while(NOT directory STREQUAL "")
            encode_paths("${directory}" encodedDirectory)
            list(APPEND staleDirectories "${encodedDirectory}")
            cmake_path(GET directory PARENT_PATH directory)
        endwhile()
    endforeach()
    list(REMOVE_DUPLICATES staleDirectories)
    list(SORT staleDirectories ORDER DESCENDING)
endif()

)LUX"
    R"LUX(function(validate_content source path)
    if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}")
        message(FATAL_ERROR "Missing generated content source: ${source}")
    endif()
    file(READ "${source}" firstByte LIMIT 1 HEX)
    content_path("${path}" destination)
    if(IS_DIRECTORY "${destination}")
        encode_paths("${path}" prefix)
        if(NOT prefix IN_LIST staleDirectories)
            message(FATAL_ERROR "Unmanaged directory blocks generated content: ${path}")
        endif()
        scan_tree("${destination}" entries)
        foreach(relative IN LISTS entries)
            set(encoded "${prefix}/${relative}")
            decode_path("${encoded}" entry)
            content_path("${entry}" candidate)
            if(IS_DIRECTORY "${candidate}")
                set(owned "${staleDirectories}")
            else()
                set(owned "${staleFiles}")
            endif()
            if(NOT encoded IN_LIST owned)
                message(FATAL_ERROR "Unmanaged content blocks directory replacement: ${entry}")
            endif()
        endforeach()
    endif()
    cmake_path(GET path PARENT_PATH directory)
    while(NOT directory STREQUAL "")
        encode_paths("${directory}" encoded)
        content_path("${directory}" candidate)
        if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}" AND
           NOT encoded IN_LIST staleFiles)
            message(FATAL_ERROR "File blocks generated content directory: ${directory}")
        endif()
        cmake_path(GET directory PARENT_PATH directory)
    endwhile()
endfunction()

foreach(key IN LISTS inputs)
    decode_path("${path_${key}}" path)
    validate_content("${input_${key}}" "${path}")
endforeach()

function(write_path_list output paths)
    set(lines "")
    foreach(encoded IN LISTS paths)
        decode_path("${encoded}" path)
        string(APPEND lines "${path}\n")
    endforeach()
    file(WRITE "${output}" "${lines}")
endfunction()

function(directory_has_entries path output)
    string(REPLACE "[" "[[]" pattern "${path}")
    string(REPLACE "*" "[*]" pattern "${pattern}")
    string(REPLACE "?" "[?]" pattern "${pattern}")
    file(GLOB entries "${pattern}/*")
    set(${output} "${entries}" PARENT_SCOPE)
endfunction()

function(clean_transaction)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -r -- "${transaction}"
                    RESULT_VARIABLE cleaned)
    if(NOT cleaned STREQUAL "0")
        message(WARNING "Could not remove content synchronization data: ${transaction}")
    endif()
endfunction()

function(rollback_content reason)
    set(failures)
    list(REVERSE installedFiles)
    foreach(index IN LISTS installedFiles)
        list(GET changedFiles ${index} encoded)
        decode_path("${encoded}" path)
        file(RENAME "${root}/${path}" "${transaction}/new/${index}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            list(APPEND failures "${path}: ${result}")
        endif()
    endforeach()
    list(REVERSE createdDirectories)
    foreach(encoded IN LISTS createdDirectories)
        decode_path("${encoded}" path)
        directory_has_entries("${root}/${path}" entries)
        if(entries OR IS_SYMLINK "${root}/${path}")
            list(APPEND failures "${path}: directory changed during synchronization")
        else()
            list(FIND createdDirectories "${encoded}" index)
            file(RENAME "${root}/${path}" "${transaction}/rollback-dirs/${index}"
                 NO_REPLACE RESULT result)
            if(NOT result STREQUAL "0")
                list(APPEND failures "${path}: ${result}")
            endif()
        endif()
    endforeach()
    list(REVERSE movedDirectories)
    foreach(index IN LISTS movedDirectories)
        list(GET existingDirectories ${index} encoded)
        decode_path("${encoded}" path)
        file(RENAME "${transaction}/old-dirs/${index}" "${path}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            list(APPEND failures "${path}: ${result}")
        endif()
    endforeach()
    foreach(index IN LISTS movedFiles)
        list(GET existingFiles ${index} encoded)
        decode_path("${encoded}" path)
        file(RENAME "${transaction}/old-files/${index}" "${path}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            list(APPEND failures "${path}: ${result}")
        endif()
    endforeach()
    if(failures)
        string(REPLACE ";" "\n" details "${failures}")
        message(FATAL_ERROR "${reason}\nRollback incomplete. Recovery files: ${transaction}\n${details}")
    endif()
    file(REMOVE "${nextManifest}")
    clean_transaction()
    message(FATAL_ERROR "${reason}\nPrevious generated content restored.")
endfunction()

set(existingFiles)
foreach(encoded IN LISTS staleFiles)
    decode_path("${encoded}" path)
    content_path("${path}" candidate)
    if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
        file(REAL_PATH "${candidate}" actual)
        encode_paths("${actual}" encoded)
        list(APPEND existingFiles "${encoded}")
    endif()
endforeach()

set(existingDirectories)
foreach(encoded IN LISTS staleDirectories)
    decode_path("${encoded}" path)
    content_path("${path}" candidate)
    if(IS_DIRECTORY "${candidate}")
        file(REAL_PATH "${candidate}" actual)
        encode_paths("${actual}" encoded)
        list(APPEND existingDirectories "${encoded}")
    endif()
endforeach()
list(REMOVE_DUPLICATES existingDirectories)
list(SORT existingDirectories ORDER DESCENDING)

set(changedFiles)
set(changedSources)
function(plan_content source path)
    encode_paths("${path}" encodedPath)
    content_path("${path}" destination)
    if(EXISTS "${destination}" AND NOT IS_DIRECTORY "${destination}")
        file(REAL_PATH "${destination}" actual)
        file(RELATIVE_PATH actualRelative "${root}" "${actual}")
        encode_paths("${actualRelative}" encodedActualRelative)
        set(owned FALSE)
        if(encodedPath IN_LIST previousPaths OR encodedActualRelative IN_LIST previousPaths)
            set(owned TRUE)
        endif()
        file(SIZE "${source}" sourceSize)
        file(SIZE "${destination}" destinationSize)
        if(sourceSize EQUAL destinationSize)
            file(SHA256 "${source}" sourceHash)
            file(SHA256 "${destination}" destinationHash)
            if(sourceHash STREQUAL destinationHash AND (actual STREQUAL destination OR NOT owned))
                return()
            endif()
        endif()
        if(NOT owned)
            message(FATAL_ERROR "Unmanaged file blocks generated content: ${path}")
        endif()
        encode_paths("${actual}" encodedActual)
        list(APPEND existingFiles "${encodedActual}")
        set(existingFiles "${existingFiles}" PARENT_SCOPE)
    endif()
    encode_paths("${source}" encodedSource)
    list(APPEND changedFiles "${encodedPath}")
    list(APPEND changedSources "${encodedSource}")
    set(changedFiles "${changedFiles}" PARENT_SCOPE)
    set(changedSources "${changedSources}" PARENT_SCOPE)
endfunction()

foreach(key IN LISTS inputs)
    decode_path("${path_${key}}" path)
    plan_content("${input_${key}}" "${path}")
endforeach()
list(REMOVE_DUPLICATES existingFiles)

list(SORT current)
if(NOT changedFiles AND NOT existingFiles AND NOT existingDirectories AND
   EXISTS "${MANIFEST}" AND previousPaths STREQUAL current)
    return()
endif()

file(MAKE_DIRECTORY "${transaction}/new" "${transaction}/old-files"
     "${transaction}/old-dirs" "${transaction}/rollback-dirs")
write_path_list("${transaction}/old-files.txt" "${existingFiles}")
write_path_list("${transaction}/old-dirs.txt" "${existingDirectories}")
write_path_list("${transaction}/new-files.txt" "${changedFiles}")
write_path_list("${nextManifest}" "${current}")

set(index 0)
foreach(encoded IN LISTS changedSources)
    decode_path("${encoded}" source)
    file(COPY_FILE "${source}" "${transaction}/new/${index}" RESULT result)
    if(NOT result STREQUAL "0")
        rollback_content("Could not stage generated content ${source}: ${result}")
    endif()
    math(EXPR index "${index} + 1")
endforeach()

file(WRITE "${transaction}/prepared" "")
set(index 0)
foreach(encoded IN LISTS existingFiles)
    decode_path("${encoded}" path)
    file(RENAME "${path}" "${transaction}/old-files/${index}" NO_REPLACE RESULT result)
    if(NOT result STREQUAL "0")
        rollback_content("Could not back up generated content ${path}: ${result}")
    endif()
    list(APPEND movedFiles "${index}")
    math(EXPR index "${index} + 1")
endforeach()

set(index 0)
foreach(encoded IN LISTS existingDirectories)
    decode_path("${encoded}" path)
    directory_has_entries("${path}" entries)
    if(NOT entries)
        file(RENAME "${path}" "${transaction}/old-dirs/${index}" NO_REPLACE RESULT result)
        if(NOT result STREQUAL "0")
            rollback_content("Could not back up generated directory ${path}: ${result}")
        endif()
        list(APPEND movedDirectories "${index}")
        directory_has_entries("${transaction}/old-dirs/${index}" movedEntries)
        if(movedEntries)
            rollback_content("Generated directory changed during synchronization: ${path}")
        endif()
    endif()
    math(EXPR index "${index} + 1")
endforeach()

set(index 0)
foreach(encoded IN LISTS changedFiles)
    decode_path("${encoded}" path)
    set(parents)
    cmake_path(GET path PARENT_PATH directory)
    while(NOT directory STREQUAL "" AND NOT IS_DIRECTORY "${root}/${directory}")
        encode_paths("${directory}" encodedDirectory)
        list(PREPEND parents "${encodedDirectory}")
        cmake_path(GET directory PARENT_PATH directory)
    endwhile()
    foreach(encodedDirectory IN LISTS parents)
        decode_path("${encodedDirectory}" directory)
        execute_process(COMMAND "${CMAKE_COMMAND}" -E make_directory "${root}/${directory}"
                        RESULT_VARIABLE result)
        if(NOT result STREQUAL "0")
            rollback_content("Could not create generated directory ${directory}: ${result}")
        endif()
        list(APPEND createdDirectories "${encodedDirectory}")
    endforeach()
    file(RENAME "${transaction}/new/${index}" "${root}/${path}" NO_REPLACE RESULT result)
    if(NOT result STREQUAL "0")
        rollback_content("Could not install generated content ${path}: ${result}")
    endif()
    list(APPEND installedFiles "${index}")
    math(EXPR index "${index} + 1")
endforeach()

file(RENAME "${nextManifest}" "${MANIFEST}" RESULT result)
if(NOT result STREQUAL "0")
    rollback_content("Could not replace generated content manifest: ${result}")
endif()
clean_transaction()
)LUX";

enum class ContentSyncVersion { Legacy, ImageSignatures, OptionalHeaders, Current };

std::string ContentSyncScript(ContentSyncVersion version = ContentSyncVersion::Current) {
    std::string script = SyncContent;
    const auto substitute = [&](std::string_view marker, std::string_view code) {
        script.replace(script.find(marker), marker.size(), code);
    };
    if (version == ContentSyncVersion::Legacy) {
        substitute("@IMAGE_CHECK@", "");
        substitute("@CONTENT_SELECTION@", LegacyContentSelection);
    } else {
        const bool optional = version != ContentSyncVersion::ImageSignatures;
        const bool current = version == ContentSyncVersion::Current;
        std::string imageCheck = optional ? ImageHeaderScript : "";
        if (current)
            imageCheck += RequiredImageCheck;
        imageCheck += ImageContentCheck;
        substitute("@IMAGE_CHECK@", imageCheck);
        substitute("@IMAGE_HEADER_READ@",
                   optional ? OptionalImageHeaderRead : DirectImageHeaderRead);
        if (optional)
            substitute("@CHECK_REFERENCED_IMAGE@",
                       current ? "        require_readable_referenced_image(\"${path}\")\n" : "");
        substitute("@CONTENT_SELECTION@", ContentSelection);
    }
    return script;
}

constexpr const char* GameSource = R"LUX(#include <LuxEngine/LuxEngine.hpp>
#include <SDL3/SDL_filesystem.h>
#include <LuxEngine/EntryPoint.hpp>
#include <LuxEngine/Scene/SceneManager.hpp>
#include <LuxEngine/Runtime/ProjectManifest.hpp>
#include <LuxEngine/Runtime/ScenePlayer.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace le = LuxEngine;

std::filesystem::path FindStartScene(const std::filesystem::path& root) {
    std::filesystem::path target;
    std::string error;
    if (!le::ReadProjectStartScene(root, target, error))
        throw std::runtime_error(error);
    return target;
}

class Game final : public le::Application {
public:
    explicit Game(bool smokeTest)
        : le::Application({.name = "LuxGame", .width = 1280, .height = 720}),
          smokeTest_(smokeTest) {}

protected:
    void OnInit() override {
        le::Renderer2D::Init(&GetGpuDevice());
        le::Renderer3D::Init(&GetGpuDevice());
        le::PhysicsEngine::Init();
        const char* base = SDL_GetBasePath();
        if (!base)
            throw std::runtime_error("게임 실행 경로를 찾을 수 없습니다.");
        const auto basePath = std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t*>(base), std::strlen(base)));
        const auto sceneFile = FindStartScene(basePath / "Content");
        std::string error;
        scene_ = manager_.Load(sceneFile, error);
        if (!scene_)
            throw std::runtime_error(error.empty() ? "시작 씬을 열 수 없습니다." : error);
        scriptAssembly_ = basePath / "Content/Scripts/GameScripts.dll";
        EnsureScripts();
        le::Input::BindAction("Left", le::LuxKey::A);
        le::Input::BindAction("Left", le::LuxKey::Left);
        le::Input::BindAction("Right", le::LuxKey::D);
        le::Input::BindAction("Right", le::LuxKey::Right);
        le::Input::BindAction("Up", le::LuxKey::W);
        le::Input::BindAction("Up", le::LuxKey::Up);
        le::Input::BindAction("Down", le::LuxKey::S);
        le::Input::BindAction("Down", le::LuxKey::Down);
        le::Input::BindAction("Action", le::LuxKey::Space);
        le::Input::BindAction("Quit", le::LuxKey::Escape);
        FindActors();
    }

    void EnsureScripts() {
        if (scripts_.IsRunning())
            return;
        for (const auto id : scene_->GetComponentEntityIds<le::ScriptComponent>()) {
            const auto& script = scene_->GetComponent<le::ScriptComponent>(id);
            if (!script.enabled || script.typeName.empty())
                continue;
            le::ScriptRuntimeConfig config;
            config.assemblyPath = scriptAssembly_;
            if (!scripts_.Initialize(*scene_, config))
                throw std::runtime_error(scripts_.GetLastError());
            return;
        }
    }

    void FindActors() {
        player_ = {};
        npc_ = {};
        goal_ = {};
        for (auto entity : scene_->GetEntities()) {
            if (!entity.HasComponent<le::TransformComponent>())
                continue;
            const auto* tagComponent = entity.TryGetComponent<le::TagComponent>();
            if (!tagComponent)
                continue;
            const auto& tag = tagComponent->tag;
            if (tag == "플레이어" || tag == "주인공" || tag == "블록")
                player_ = entity;
            if (tag == "NPC")
                npc_ = entity;
            if (tag == "목표")
                goal_ = entity;
        }
    }

    void TickStreaming(double dt) {
        streamingElapsed_ += dt;
        if (streamingElapsed_ < .1)
            return;
        streamingElapsed_ = 0;
        const auto observer = le::ResolveSceneStreamingObserver(*scene_, {}, player_.GetId());
        std::string error;
        bool changed = false;
        if (!manager_.UpdateStreaming(*scene_, {observer.x, observer.y}, error, &changed)) {
            if (error != streamingError_)
                le::Log::Error(error);
            streamingError_ = std::move(error);
        } else if (changed) {
            streamingError_.clear();
        }
        if (changed) {
            FindActors();
            EnsureScripts();
        }
    }

    double GetFixedUpdateStep() const override {
        return scene_->GetSettings().fixedStep;
    }

    void OnUpdate(double dt) override {
        if (le::Input::WasActionPressed("Quit"))
            Close();
        const auto* script = player_.TryGetComponent<le::ScriptComponent>();
        if (player_ && (!script || !script->enabled || script->typeName.empty())) {
            const float horizontal = (le::Input::IsActionDown("Right") ? 1.0f : 0.0f) -
                                     (le::Input::IsActionDown("Left") ? 1.0f : 0.0f);
            const float vertical = (le::Input::IsActionDown("Up") ? 1.0f : 0.0f) -
                                   (le::Input::IsActionDown("Down") ? 1.0f : 0.0f);
            auto& transform = player_.GetComponent<le::TransformComponent>();
            if constexpr (LUX_GAME_TEMPLATE_KIND == 1) {
                if (auto* body = player_.TryGetComponent<le::Rigidbody2DComponent>()) {
                    body->velocity.x = horizontal * 4.0f;
                    if (body->isGrounded && le::Input::WasActionPressed("Action"))
                        body->velocity.y = 7.0f;
                }
            } else if constexpr (LUX_GAME_TEMPLATE_KIND == 4) {
                if (le::Input::WasActionPressed("Left"))
                    transform.position.x -= 1.0f;
                if (le::Input::WasActionPressed("Right"))
                    transform.position.x += 1.0f;
                if (le::Input::WasActionPressed("Up"))
                    transform.position.y += 1.0f;
                if (le::Input::WasActionPressed("Down"))
                    transform.position.y -= 1.0f;
                transform.position.x = std::clamp(transform.position.x, -3.0, 3.0);
                transform.position.y = std::clamp(transform.position.y, -3.0, 3.0);
                if (goal_) {
                    const auto& target = goal_.GetComponent<le::TransformComponent>().position;
                    if (transform.position.x == target.x && transform.position.y == target.y) {
                        if (auto* sprite = player_.TryGetComponent<le::SpriteRendererComponent>())
                            sprite->color = {0.42f, 0.95f, 0.58f, 1.0f};
                    }
                }
            } else {
                const float length = std::sqrt(horizontal * horizontal + vertical * vertical);
                if (length > 0.0f) {
                    transform.position.x += horizontal / length * 4.0f * static_cast<float>(dt);
                    transform.position.y += vertical / length * 4.0f * static_cast<float>(dt);
                }
            }
            if constexpr (LUX_GAME_TEMPLATE_KIND == 3) {
                if (npc_ && le::Input::WasActionPressed("Action")) {
                    const auto& target = npc_.GetComponent<le::TransformComponent>().position;
                    if (std::abs(target.x - transform.position.x) < 1.5f &&
                        std::abs(target.y - transform.position.y) < 1.5f) {
                        if (auto* sprite = npc_.TryGetComponent<le::SpriteRendererComponent>())
                            sprite->color = sprite->color.x > 0.5f
                                                ? le::Vec4{0.34f, 0.78f, 0.54f, 1.0f}
                                                : le::Vec4{0.82f, 0.52f, 0.38f, 1.0f};
                    }
                }
            }
        }
        TickStreaming(dt);
        if (!le::UpdateSceneSimulation(*scene_, scripts_, dt))
            throw std::runtime_error(scripts_.GetLastError());
        if (smokeTest_ && ++frames_ >= 10) {
            if (GetGpuDevice().GetCounters().validationErrors != 0)
                throw std::runtime_error("Vulkan 검증에서 렌더링 오류가 발견되었습니다.");
            Close();
        }
    }

    void OnRender(float) override {
        const auto& settings = scene_->GetSettings();
        GetGpuDevice().Clear(settings.clearColor.x, settings.clearColor.y, settings.clearColor.z,
                             settings.clearColor.w);
        le::uint32 width = 0, height = 0;
        const auto aspect = GetWindow().GetFramebufferSize(width, height) && width && height
                                ? static_cast<double>(width) / height
                                : 16.0 / 9;
        auto cameras = le::ResolveScenePlayerCameras(*scene_, aspect, {});
        if (!cameras.cameraEntity) {
            cameras.camera3D.SetPosition({0, 6, 12});
            cameras.camera3D.SetTarget({0, 0, 0});
        }
        if (cameras.perspective || !cameras.cameraEntity) {
            auto renderSettings = settings.render3D;
            if (!renderSettings.environmentPath.empty())
                renderSettings.environmentPath =
                    scene_->ResolveAssetPath(renderSettings.environmentPath);
            le::Renderer3D::BeginScene(cameras.camera3D, renderSettings, settings.exposure);
            scene_->OnRender3D();
            le::Renderer3D::EndScene();
        }
        le::Renderer2D::BeginScene(cameras.camera2D);
        scene_->OnRender();
        le::Renderer2D::EndScene();
    }

    void OnShutdown() override {
        scripts_.Shutdown();
        le::Input::ClearBindings();
        manager_.ClearCache();
        le::PhysicsEngine::Shutdown();
        le::Renderer3D::Shutdown();
        le::Renderer2D::Shutdown();
    }

private:
    le::SceneManager manager_;
    le::ScriptRuntime scripts_;
    std::filesystem::path scriptAssembly_;
    std::shared_ptr<le::Scene> scene_;
    le::Entity player_;
    le::Entity npc_;
    le::Entity goal_;
    double streamingElapsed_ = 0;
    std::string streamingError_;
    bool smokeTest_ = false;
    int frames_ = 0;
};

int RunGame(int argc, char* argv[]) {
    Game game(argc > 1 && std::string_view(argv[1]) == "--smoke-test");
    if (!game.Initialize())
        return 1;
    const int result = game.Run();
    game.Shutdown();
    return result;
}
LUX_ENGINE_MAIN(RunGame)
)LUX";
constexpr const char* ScriptProject = R"LUX(<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <Nullable>enable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
  </PropertyGroup>
  <ItemGroup>
    <Reference Include="LuxEngine.ScriptCore">
      <HintPath>$(LuxEngineScriptCore)</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
)LUX";

constexpr const char* ScriptSource = R"LUX(using LuxEngine;

namespace Game {
    public sealed class Mover : EntityScript {
        public float Speed = 4.0f;

        protected override void OnUpdate(double deltaTime) {
            Transform transform = Entity.Transform;
            float horizontal = (Input.IsKeyPressed(LuxKey.D) ? 1.0f : 0.0f)
                - (Input.IsKeyPressed(LuxKey.A) ? 1.0f : 0.0f);
            transform.Position.X += horizontal * Speed * (float)deltaTime;
            Entity.Transform = transform;
        }
    }
}
)LUX";

constexpr const char* ScriptFormatting = R"LUX(root = true

[*.cs]
indent_style = space
indent_size = 4
end_of_line = lf
charset = utf-8
insert_final_newline = true
csharp_new_line_before_open_brace = none
csharp_new_line_before_else = false
csharp_new_line_before_catch = false
csharp_new_line_before_finally = false
csharp_style_namespace_declarations = block_scoped:warning
csharp_style_expression_bodied_methods = false:warning
csharp_preserve_single_line_blocks = false
csharp_preserve_single_line_statements = false
)LUX";
} // namespace

bool CreateTemplateFiles(const std::filesystem::path& directory, ProjectTemplate projectTemplate,
                         std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(directory / "Scripts", ec);
    if (ec) {
        error = "프로젝트 소스 폴더를 만들 수 없습니다.";
        return false;
    }
    std::filesystem::create_directories(directory / "Source", ec);
    if (ec) {
        error = "프로젝트 소스 폴더를 만들 수 없습니다.";
        return false;
    }
    const auto syncContent = ContentSyncScript();
    const std::pair<const char*, std::string_view> scriptFiles[] = {
        {"Scripts/GameScripts.csproj", ScriptProject},
        {"Scripts/Mover.cs", ScriptSource},
        {".editorconfig", ScriptFormatting},
        {"SyncContent.cmake", syncContent},
        {"Scripts/NuGet.Config",
         "<configuration><packageSources><clear /></packageSources></configuration>\n"}};
    for (const auto& [path, content] : scriptFiles) {
        std::ofstream file(directory / path, std::ios::binary);
        file << content;
        file.close();
        if (!file) {
            error = "C# 프로젝트 파일을 저장할 수 없습니다.";
            return false;
        }
    }
    std::string cmake = GameCMake;
    const auto marker = cmake.find("@KIND@");
    cmake.replace(marker, 6, std::to_string(static_cast<int>(projectTemplate)));
    std::ofstream buildFile(directory / "CMakeLists.txt", std::ios::binary);
    std::ofstream sourceFile(directory / "Source" / "Main.cpp", std::ios::binary);
    buildFile << cmake;
    sourceFile << GameSource;
    buildFile.close();
    sourceFile.close();
    if (!buildFile || !sourceFile) {
        error = "프로젝트 시작 코드를 저장할 수 없습니다.";
        return false;
    }
    return true;
}

bool UpgradeTemplateContentSync(const std::filesystem::path& directory, std::string& error) {
    const auto path = directory / "SyncContent.cmake";
    std::error_code ec;
    if (std::filesystem::is_symlink(path, ec) || ec)
        return true;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return true;
    const auto legacy = ContentSyncScript(ContentSyncVersion::Legacy);
    const auto signatures = ContentSyncScript(ContentSyncVersion::ImageSignatures);
    const auto optionalHeaders = ContentSyncScript(ContentSyncVersion::OptionalHeaders);
    if (size != legacy.size() && size != signatures.size() && size != optionalHeaders.size())
        return true;
    std::string existing(static_cast<size_t>(size), '\0');
    std::ifstream input(path, std::ios::binary);
    if (!input.read(existing.data(), static_cast<std::streamsize>(existing.size())) ||
        input.peek() != std::char_traits<char>::eof() ||
        (existing != legacy && existing != signatures && existing != optionalHeaders))
        return true;
    input.close();

    LuxUI::SaveFile destination(LuxUI::String::fromStdU16String(path.u16string()));
    const auto script = ContentSyncScript();
    const auto scriptSize = static_cast<qint64>(script.size());
    if (!destination.open(LuxUI::IODevice::WriteOnly) ||
        destination.write(script.data(), scriptSize) != scriptSize || !destination.commit()) {
        error = "프로젝트 콘텐츠 복사 스크립트를 갱신할 수 없습니다.";
        return false;
    }
    return true;
}

} // namespace LuxEditor
