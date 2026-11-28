lux_add_sample(SampleFrontierCounty NO_CLI
    Main.cpp Characters.cpp PropModels.cpp World.cpp SurfaceMarks.cpp Game.cpp RenderDebugPanel.cpp)
add_dependencies(SampleFrontierCounty RainyCreekAssets)

# Use the vendored Dear ImGui core with a standalone configuration. The Forge's
# imconfig.h couples ImGui to its private allocators, file system and vector types.
set(_forge_common "${CMAKE_SOURCE_DIR}/Engine/ThirdParty/TheForge/Common_3")
set(_frontier_imgui "${_forge_common}/Application/ThirdParty/OpenSource/imgui")
set(_frontier_imgui_build "${CMAKE_CURRENT_BINARY_DIR}/imgui")
file(MAKE_DIRECTORY "${_frontier_imgui_build}")
set(_frontier_imgui_files imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp imgui.h
    imgui_internal.h)
foreach(_file IN LISTS _frontier_imgui_files)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_frontier_imgui}/${_file}")
    file(READ "${_frontier_imgui}/${_file}" _source)
    string(REPLACE "../../../../Utilities/ThirdParty/OpenSource/Nothings/stb_textedit.h"
        "stb_textedit.h" _source "${_source}")
    file(CONFIGURE OUTPUT "${_frontier_imgui_build}/${_file}" CONTENT "${_source}" @ONLY)
endforeach()
file(CONFIGURE OUTPUT "${_frontier_imgui_build}/imconfig.h" CONTENT [=[
#pragma once
#define IMGUI_STB_RECT_PACK_FILENAME "stb_rectpack.h"
#define IMGUI_STB_TRUETYPE_FILENAME "stb_truetype.h"
#define IMGUI_DISABLE_WIN32_FUNCTIONS
]=] @ONLY)
add_library(FrontierImGui STATIC
    "${_frontier_imgui_build}/imgui.cpp"
    "${_frontier_imgui_build}/imgui_draw.cpp"
    "${_frontier_imgui_build}/imgui_tables.cpp"
    "${_frontier_imgui_build}/imgui_widgets.cpp")
target_include_directories(FrontierImGui SYSTEM
    PUBLIC "${_frontier_imgui_build}"
    PRIVATE "${_forge_common}/Utilities/ThirdParty/OpenSource/Nothings")
target_link_libraries(SampleFrontierCounty PRIVATE FrontierImGui)
