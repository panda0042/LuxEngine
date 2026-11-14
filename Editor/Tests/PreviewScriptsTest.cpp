#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Viewport/GameViewport.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    using namespace LuxEditor;
    using namespace LuxEngine;
    LuxUI::Application app(argc, argv);
    LuxUI::CoreApplication::setOrganizationName(LUX_UI_TEXT("LuxEngineTests"));
    LuxUI::CoreApplication::setApplicationName(LUX_UI_TEXT("PreviewScripts"));
    LuxUI::TemporaryDir files;
    LuxUI::Settings::setDefaultFormat(LuxUI::Settings::IniFormat);
    LuxUI::Settings::setPath(LuxUI::Settings::IniFormat, LuxUI::Settings::UserScope, files.path());
    try {
        Check(argc == 2 && files.isValid(), "script assembly and temporary directory required");
        const auto root = PathFromUtf8(files.path().toUtf8().toStdString());
        std::string error;
        auto project =
            EditorProject::Create(root / "Project", "Preview", ProjectTemplate::Empty, error);
        Check(project.has_value(), error.c_str());
        const auto managed = project->GetFile().parent_path() / ".lux/managed";
        std::filesystem::create_directories(managed);
        std::filesystem::copy_file(PathFromUtf8(argv[1]), managed / "GameScripts.dll");
        LuxUI::Settings().setValue(LUX_UI_TEXT("lastProject"),
                                   LuxUI::String::fromUtf8(PathUtf8(project->GetFile())));
        LuxUI::Settings().setValue(LUX_UI_TEXT("lastScene"),
                                   LuxUI::String::fromUtf8(PathUtf8(project->GetSceneFile())));
        EditorWindow window;
        window.show();
        app.processEvents();
        auto& document = window.GetDocument();
        auto actor = document.CreateEntity("Preview script actor");
        actor.AddComponent<ScriptComponent>() = {"ScriptTests.Mover", true};
        const auto id = actor.GetId();
        auto settings = document.GetScene().GetSettings();
        settings.fixedStep = 1.0f / 73;
        Check(document.GetScene().SetSettings(settings), "custom fixed step accepted");
        const auto* authored = &document.GetScene();
        const bool dirty = document.IsDirty();
        auto* history = window.findChild<LuxUI::UndoStack*>();
        auto* view = dynamic_cast<GameViewport*>(
            window.findChild<LuxUI::Widget*>(LUX_UI_TEXT("gameViewport")));
        Check(history && view, "history and Game View exist");
        const int historyIndex = history->index();
        const auto trigger = [&](const char* name) {
            auto* action = window.findChild<LuxUI::Action*>(LuxUI::String::fromLatin1(name));
            Check(action && action->isEnabled(), name);
            action->trigger();
        };
        for (int cycle = 0; cycle < 3; ++cycle) {
            trigger("previewStep");
            Check(window.GetRuntimeScene() && window.GetRuntimeScene() != authored &&
                      view->IsPaused(),
                  "step creates a paused isolated runtime using the project's generated assembly");
            const auto position =
                window.GetRuntimeScene()->GetComponent<TransformComponent>(id).position;
            Check(position.y == 10 && std::abs(position.x - settings.fixedStep) < 1e-7f,
                  "OnCreate and exactly one OnUpdate use the configured scene fixed step");
            app.processEvents();
            Check(window.GetRuntimeScene()->GetComponent<TransformComponent>(id).position.x ==
                      position.x,
                  "paused scripts do not advance during UI events");
            trigger("previewStep");
            Check(
                std::abs(window.GetRuntimeScene()->GetComponent<TransformComponent>(id).position.x -
                         2 * settings.fixedStep) < 1e-7f,
                "single stepping preserves the existing script instance");
            trigger("previewStop");
            const auto original = actor.GetComponent<TransformComponent>().position;
            Check(
                !window.GetRuntimeScene() && &document.GetScene() == authored && actor.IsValid() &&
                    original.x == 0 && original.y == 0 && original.z == 0 &&
                    document.IsDirty() == dirty && history->index() == historyIndex,
                "stop isolates OnDestroy and preserves authored identity, dirty state and history");
        }
        actor.GetComponent<ScriptComponent>().typeName = "ScriptTests.Throwing";
        trigger("previewStep");
        Check(window.GetRuntimeScene() && view->IsPaused() &&
                  window.LastLogMessage().contains(LUX_UI_TEXT("managed regression exception")),
              "script errors pause and preserve the runtime with diagnostics");
        trigger("previewStop");
        Check(&document.GetScene() == authored && actor.IsValid(),
              "error recovery preserves original scene");
        document.SetDirty(false);
        std::cout << "Generated assembly, fixed-step scripts, repeated play, pause, stop and "
                     "errors passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
