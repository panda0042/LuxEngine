#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/App/PreviewSession.hpp>
#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Viewport/GameViewport.hpp>
#include <LuxEngine/Animation/AnimationEngine.hpp>
#include <LuxEngine/Core/Log.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void CheckPreviewSessionTiming() {
    using namespace LuxEngine;
    Scene authored;
    SceneManager sceneManager;
    LuxEditor::PreviewSession preview(sceneManager);
    Check(!preview.Start(authored, {}) && preview.IsRunning(), "preview starts without Qt widgets");
    const auto step = static_cast<double>(authored.GetSettings().fixedStep);
    constexpr auto limit = static_cast<std::uint64_t>(FixedStepAccumulator::MaxUpdatesPerFrame);
    const auto observe = [](const LuxEditor::PreviewSession::Update& update) {
        Check(!update.failure && update.streamingError.empty(),
              "blank scene advances successfully");
    };
    preview.Advance(step * (static_cast<double>(limit) + 4.5), {}, observe);
    Check(preview.Frame() == limit, "preview caps the work of a delayed update");
    preview.Advance(0, {}, observe);
    Check(preview.Frame() == limit, "preview drops the excess whole steps");
    preview.Advance(step * .75, {}, observe);
    Check(preview.Frame() == limit, "preview retains only a partial step");

    preview.Pause();
    preview.Advance(step * 10, {}, observe);
    Check(preview.Frame() == limit && preview.IsPaused(), "paused preview ignores elapsed time");
    preview.Step({}, observe);
    Check(preview.Frame() == limit + 1 && preview.IsPaused(), "manual preview step advances once");
    preview.Resume();
    preview.Advance(step * .5, {}, observe);
    Check(preview.Frame() == limit + 1 && preview.IsRunning(), "resume resets accumulated time");
    preview.Advance(step * .75, {}, observe);
    Check(preview.Frame() == limit + 2, "resumed preview advances after a complete step");
    preview.Stop();
    Check(!preview.IsActive() && !preview.GetScene(), "stop releases the runtime scene");
}

int main(int argc, char** argv) {
    using namespace LuxEditor;
    using namespace LuxEngine;
    LuxUI::Application::setAttribute(LuxUI::Flags::AA_DontUseNativeDialogs);
    LuxUI::Application app(argc, argv);
    LuxUI::CoreApplication::setOrganizationName(LUX_UI_TEXT("LuxEngineTests"));
    LuxUI::CoreApplication::setApplicationName(LUX_UI_TEXT("PreviewScripts"));
    LuxUI::TemporaryDir files;
    LuxUI::Settings::setDefaultFormat(LuxUI::Settings::IniFormat);
    LuxUI::Settings::setPath(LuxUI::Settings::IniFormat, LuxUI::Settings::UserScope, files.path());
    try {
        Check(argc == 2 && files.isValid(), "script assembly and temporary directory required");
        CheckPreviewSessionTiming();
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
        const auto primaryTab = document.GetActiveTabId();
        const auto secondaryPath = project->GetSceneFile().parent_path() / "Secondary.lux";
        Scene secondary;
        Check(secondary.Serialize(PathUtf8(secondaryPath)) &&
                  window.OpenSceneDocument(secondaryPath),
              "secondary scene opens in another tab");
        const auto secondaryTab = document.GetActiveTabId();
        const auto secondaryEntity = document.CreateEntity("Saved during preview").GetId();
        Check(secondaryTab != primaryTab && window.SwitchSceneDocument(primaryTab, false),
              "switch back to the primary scene");
        auto actor = document.CreateEntity("Preview script actor");
        actor.AddComponent<ScriptComponent>() = {"ScriptTests.Mover", true};
        const auto id = actor.GetId();
        auto animated = document.CreateEntity("Preview animation actor");
        auto& animation = animated.AddComponent<AnimationComponent>();
        animation.clip.duration = 2;
        animation.clip.AddKeyframe({0, {0, 0, 0}});
        animation.clip.AddKeyframe({2, {2, 0, 0}});
        animation.currentTime = .75f;
        const auto animatedId = animated.GetId();
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
            Check(std::abs(window.GetRuntimeScene()
                               ->GetComponent<AnimationComponent>(animatedId)
                               .currentTime -
                           (.75f + settings.fixedStep)) < 1e-6f &&
                      animation.currentTime == .75f,
                  "play advances from the authored animation time without changing its source");
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
        trigger("previewStart");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        app.processEvents();
        Check(window.GetRuntimeScene() &&
                  window.GetRuntimeScene()->GetComponent<TransformComponent>(id).position.x >=
                      3 * settings.fixedStep,
              "the Game View catches up after a delayed event loop instead of advancing one tick");
        trigger("previewStop");
        const auto* console =
            window.findChild<LuxUI::PlainTextEdit*>(LUX_UI_TEXT("luxConsoleOutput"));
        Check(console, "development console exists");
        const auto waitForLog = [&](const LuxUI::String& message) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!console->toPlainText().contains(message) &&
                   std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                app.processEvents();
            }
            Check(console->toPlainText().contains(message), "engine error reaches the console");
        };
        Log::Error("preview log from before start");
        trigger("previewStart");
        waitForLog(LUX_UI_TEXT("preview log from before start"));
        Check(!view->IsPaused(), "an earlier engine error does not pause a new preview");
        Log::Error("preview log from the active session");
        waitForLog(LUX_UI_TEXT("preview log from the active session"));
        Check(view->IsPaused(), "an engine error in the active session pauses preview");
        trigger("previewStop");
        trigger("previewStart");
        Log::Error("preview log from the stopped session");
        trigger("previewStop");
        trigger("previewStart");
        waitForLog(LUX_UI_TEXT("preview log from the stopped session"));
        Check(window.GetRuntimeScene() && !view->IsPaused(),
              "a queued error from a stopped preview does not pause the next session");
        trigger("previewStop");
        actor.GetComponent<ScriptComponent>().typeName = "ScriptTests.Throwing";
        trigger("previewStep");
        Check(window.GetRuntimeScene() && view->IsPaused() &&
                  window.LastLogMessage().contains(LUX_UI_TEXT("managed regression exception")),
              "script errors pause and preserve the runtime with diagnostics");
        trigger("previewStop");
        Check(&document.GetScene() == authored && actor.IsValid(),
              "error recovery preserves original scene");
        actor.GetComponent<ScriptComponent>().typeName = "ScriptTests.Mover";
        std::filesystem::rename(managed / "GameScripts.dll", managed / "MissingScripts.dll");
        trigger("previewStep");
        Check(!window.GetRuntimeScene() && &document.GetScene() == authored && actor.IsValid() &&
                  window.findChild<LuxUI::Action*>(LUX_UI_TEXT("previewStart"))->isEnabled(),
              "missing assembly leaves the authored scene and play controls usable");
        std::filesystem::rename(managed / "MissingScripts.dll", managed / "GameScripts.dll");
        trigger("previewStep");
        Check(window.GetRuntimeScene() && view->IsPaused(),
              "play recovers after restoring the missing assembly");
        auto* running = window.GetRuntimeScene();
        trigger("saveScene");
        Scene saved;
        Check(window.GetRuntimeScene() == running && view->IsPaused() && !document.IsDirty() &&
                  saved.Deserialize(document.GetPath()) &&
                  saved.GetComponent<TransformComponent>(id).position.y == 0 &&
                  running->GetComponent<TransformComponent>(id).position.y == 10,
              "the save action writes the authored scene and preserves the paused preview");

        const auto originalPath = document.GetPath();
        LuxUI::Timer chooser;
        bool cancelled = false;
        const auto dialogDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        LuxUI::Object::connect(&chooser, &LuxUI::Timer::timeout, &window, [&] {
            auto* modal = LuxUI::Application::activeModalWidget();
            if (auto* dialog = qobject_cast<LuxUI::FileDialog*>(modal)) {
                cancelled = true;
                dialog->reject();
            } else if (std::chrono::steady_clock::now() >= dialogDeadline) {
                if (auto* dialog = qobject_cast<LuxUI::Dialog*>(modal))
                    dialog->reject();
            }
        });
        chooser.start(5);
        trigger("saveSceneAs");
        chooser.stop();
        Check(cancelled && document.GetPath() == originalPath &&
                  window.GetRuntimeScene() == running && view->IsPaused(),
              "cancelling the save dialog preserves the authored path and paused preview");

        trigger("saveAllScenes");
        Scene savedSecondary;
        Check(document.GetActiveTabId() == primaryTab && window.GetRuntimeScene() == running &&
                  view->IsPaused() && !document.IsDirty(secondaryTab) &&
                  savedSecondary.Deserialize(PathUtf8(secondaryPath)) &&
                  savedSecondary.IsAlive(secondaryEntity),
              "save all writes inactive tabs without changing the active tab or preview");

        const auto localEdit = document.CreateEntity("Unsaved local edit").GetId();
        Scene external;
        const auto externalEdit = external.CreateEntity("External file edit").GetId();
        Check(external.Serialize(originalPath), "simulate an external scene file update");
        LuxUI::Timer dismissSaveError;
        bool showedSaveError = false;
        LuxUI::Object::connect(&dismissSaveError, &LuxUI::Timer::timeout, &window, [&] {
            if (auto* dialog =
                    qobject_cast<LuxUI::Dialog*>(LuxUI::Application::activeModalWidget())) {
                showedSaveError = dialog->windowTitle() == LUX_UI_TEXT("저장 실패");
                dialog->accept();
            }
        });
        dismissSaveError.start(5);
        trigger("saveScene");
        dismissSaveError.stop();
        Check(showedSaveError && window.LastLogMessage().contains(LUX_UI_TEXT("외부에서 변경")) &&
                  window.GetRuntimeScene() == running && view->IsPaused() && document.IsDirty() &&
                  document.GetPath() == originalPath && document.GetScene().IsAlive(localEdit),
              "a save conflict preserves the paused preview and unsaved authored edits");
        Scene diskAfterConflict;
        Check(diskAfterConflict.Deserialize(originalPath) &&
                  diskAfterConflict.GetComponent<TagComponent>(externalEdit).tag ==
                      "External file edit",
              "a save conflict leaves the external file untouched");
        trigger("previewStop");
        trigger("playWindow");
        auto* player = window.findChild<LuxUI::Process*>();
        Check(player, "external player process must be created");
        const auto arguments = player->arguments();
        const int scriptOption = arguments.indexOf(LUX_UI_TEXT("--scripts"));
        const bool hasAssembly = scriptOption >= 0 && scriptOption + 1 < arguments.size() &&
                                 PathFromUtf8(arguments[scriptOption + 1].toUtf8().toStdString()) ==
                                     managed / "GameScripts.dll";
        trigger("playWindowStop");
        Check(hasAssembly, "external player must receive the project's C# assembly path");
        actor.AddComponent<BoxCollider3DComponent>();
        actor.AddComponent<Rigidbody3DComponent>();
        trigger("previewStep");
        window.GetRuntimeScene()->GetComponent<Rigidbody3DComponent>(id).mass = -1;
        trigger("previewStep");
        Check(view->IsPaused() &&
                  window.LastLogMessage().contains(LUX_UI_TEXT("Jolt physics entity #")),
              "runtime physics errors pause with an entity diagnostic");
        window.GetRuntimeScene()->GetComponent<Rigidbody3DComponent>(id).mass = 1;
        trigger("previewStep");
        Check(window.GetRuntimeScene()->GetLastPhysicsError3D().empty(),
              "runtime physics can recover after repairing an invalid body");
        trigger("previewStop");
        document.SetDirty(false);
        std::cout << "Generated assembly, fixed-step scripts, repeated play, pause, stop and "
                     "errors passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
