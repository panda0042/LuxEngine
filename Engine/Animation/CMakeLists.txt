lux_engine_module(Animation
    PUBLIC_DEPENDENCIES LuxModuleCore LuxModuleECS
    PRIVATE_DEPENDENCIES nlohmann_json::nlohmann_json
    HEADERS
        Public/LuxEngine/Animation/AnimationEngine.hpp
    SOURCES
        Private/AnimationEngine.cpp
        Private/AnimationData.cpp
        Private/AnimationAssets.cpp
        Private/Animator.cpp
)
