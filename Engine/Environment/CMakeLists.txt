lux_engine_module(Environment
    PUBLIC_DEPENDENCIES LuxModuleCore
    HEADERS
        Public/LuxEngine/Environment/Atmosphere.hpp
        Public/LuxEngine/Environment/EnvironmentMap.hpp
    SOURCES
        Private/Atmosphere.cpp
        Private/EnvironmentMap.cpp
)
target_include_directories(LuxModuleEnvironment SYSTEM PRIVATE "${stb_SOURCE_DIR}")
