target_sources(LuxEditorUi PRIVATE
    Private/LuxEditor/Scripting/ScriptWorkspace.cpp
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
