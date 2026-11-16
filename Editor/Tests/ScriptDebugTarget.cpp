#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <LuxEngine/Scripting/ScriptRuntime.hpp>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    LuxEngine::Scene scene;
    auto entity = scene.CreateEntity("Script Debug Target");
    entity.AddComponent<LuxEngine::ScriptComponent>().typeName = "ScriptTests.DebugProbe";
    LuxEngine::ScriptRuntime runtime;
    LuxEngine::ScriptRuntimeConfig config;
    config.assemblyPath = argv[1];
    if (!runtime.Initialize(scene, config)) {
        std::cerr << runtime.GetLastError();
        return 1;
    }
    for (int frame = 0; frame < 200; ++frame) {
        if (!runtime.Update(.025)) {
            std::cerr << runtime.GetLastError();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    runtime.Shutdown();
    return 0;
}
