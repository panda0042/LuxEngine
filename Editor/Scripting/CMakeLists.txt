target_sources(LuxEditorUi PRIVATE
    Private/LuxEditor/Scripting/ScriptWorkspace.cpp
    Private/LuxEditor/Scripting/ToolProtocol.cpp
    Private/LuxEditor/Scripting/CSharpLanguageClient.cpp
    Private/LuxEditor/Scripting/ScriptDebugger.cpp
    Private/LuxEditor/Scripting/ScriptIntelligence.cpp
    Private/LuxEditor/Scripting/ScriptDebugWorkspace.cpp
)

if(LUX_BUILD_TESTS)
    add_executable(LuxScriptWorkspaceTests ../Tests/ScriptWorkspaceTest.cpp)
    target_link_libraries(LuxScriptWorkspaceTests PRIVATE LuxEditorUi)
    if(MSVC)
        target_compile_options(LuxScriptWorkspaceTests PRIVATE /utf-8)
    endif()
    target_include_directories(LuxScriptWorkspaceTests PRIVATE ${LUX_EDITOR_PRIVATE_INCLUDE_DIRS})
    lux_editor_copy_runtime(LuxScriptWorkspaceTests)
    lux_editor_copy_qt_plugins(LuxScriptWorkspaceTests)
    add_test(NAME Editor_script_workspace COMMAND LuxScriptWorkspaceTests)
    set_tests_properties(Editor_script_workspace PROPERTIES LABELS "headless;editor;scripting"
        TIMEOUT 60 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LUX_EDITOR_SOFTWARE_VIEWPORT=1;DOTNET_ROOT=${LUX_DOTNET_ROOT};DOTNET_ROOT_X64=${LUX_DOTNET_ROOT}")
endif()

# Optional integration tests use developer-installed tools; configuring never downloads them.
if(LUX_BUILD_TESTS)
    find_program(LUX_TEST_CSHARP_LS NAMES csharp-ls HINTS "${CMAKE_SOURCE_DIR}/.deps/script-tools")
    find_program(LUX_TEST_NETCOREDBG NAMES netcoredbg
        HINTS "${CMAKE_SOURCE_DIR}/.deps/script-tools/debugger/netcoredbg")
    if(LUX_TEST_CSHARP_LS AND LUX_TEST_NETCOREDBG)
        set(_lux_debug_scripts "${CMAKE_BINARY_DIR}/test-debug-scripts")
        add_custom_command(OUTPUT "${_lux_debug_scripts}/Scripts.dll"
            COMMAND "${LUX_DOTNET_EXECUTABLE}" build "${CMAKE_SOURCE_DIR}/Engine/Tests/Scripting/Scripts.csproj"
                --configuration Debug --output "${_lux_debug_scripts}"
                --artifacts-path "${CMAKE_BINARY_DIR}/script-debug-artifacts"
                "-p:LuxEngineScriptCore=${LUX_MANAGED_OUTPUT}/LuxEngine.ScriptCore.dll"
                "-p:RestoreConfigFile=${CMAKE_SOURCE_DIR}/Engine/Scripting/Managed/NuGet.Config"
                -p:NuGetAudit=false
            DEPENDS LuxScriptCore "${CMAKE_SOURCE_DIR}/Engine/Tests/Scripting/Scripts.csproj"
                "${CMAKE_SOURCE_DIR}/Engine/Tests/Scripting/Scripts.cs" VERBATIM)
        add_custom_target(LuxDebugScripts DEPENDS "${_lux_debug_scripts}/Scripts.dll")
        add_executable(LuxScriptDebugTarget ../Tests/ScriptDebugTarget.cpp)
        target_link_libraries(LuxScriptDebugTarget PRIVATE LuxEngine::Engine)
        lux_editor_copy_runtime(LuxScriptDebugTarget)
        add_dependencies(LuxScriptDebugTarget LuxDebugScripts)
        add_executable(LuxScriptToolsTests ../Tests/ScriptToolsTest.cpp)
        target_link_libraries(LuxScriptToolsTests PRIVATE LuxEditorUi)
        target_include_directories(LuxScriptToolsTests PRIVATE ${LUX_EDITOR_PRIVATE_INCLUDE_DIRS})
        lux_editor_copy_runtime(LuxScriptToolsTests)
        lux_editor_copy_qt_plugins(LuxScriptToolsTests)
        add_dependencies(LuxScriptToolsTests LuxScriptDebugTarget)
        add_test(NAME Editor_script_tools COMMAND LuxScriptToolsTests
            "$<TARGET_FILE:LuxScriptDebugTarget>" "${_lux_debug_scripts}/Scripts.dll"
            "${CMAKE_SOURCE_DIR}/Engine/Tests/Scripting/Scripts.cs")
        set_tests_properties(Editor_script_tools PROPERTIES LABELS "headless;editor;scripting;integration"
            TIMEOUT 90 ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LUX_CSHARP_LS=${LUX_TEST_CSHARP_LS};LUX_NETCOREDBG=${LUX_TEST_NETCOREDBG}")
    endif()
endif()
