set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release;MinSizeRel;Debug;None;NOCONFIG;")
set(CMAKE_MAP_IMPORTED_CONFIG_MINSIZEREL "MinSizeRel;Release;RelWithDebInfo;Debug;None;NOCONFIG;")
set(CMAKE_MAP_IMPORTED_CONFIG_RELEASE "Release;RelWithDebInfo;MinSizeRel;Debug;None;NOCONFIG;")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG "Debug;Release;RelWithDebInfo;MinSizeRel;None;NOCONFIG;")

function(lux_configure_jolt_imports)
    set_target_properties(Jolt::Jolt PROPERTIES
        MAP_IMPORTED_CONFIG_DEBUG Debug
        MAP_IMPORTED_CONFIG_RELEASE Release
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO "RelWithDebInfo;Release"
        MAP_IMPORTED_CONFIG_MINSIZEREL "MinSizeRel;Release")
endfunction()
