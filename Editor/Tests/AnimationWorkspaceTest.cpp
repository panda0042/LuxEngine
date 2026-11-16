#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Viewport/EngineSceneRenderer.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void Mouse(LuxUI::Widget* target, LuxUI::Event::Type type, LuxUI::PointF point,
           LuxUI::Flags::MouseButton button, LuxUI::Flags::MouseButtons buttons) {
    LuxUI::MouseEvent event(type, point, point, button, buttons, LuxUI::Flags::NoModifier);
    LuxUI::Application::sendEvent(target, &event);
}
} // namespace

int main(int argc, char** argv) {
    LuxUI::Application app(argc, argv);
    LuxUI::CoreApplication::setOrganizationName(LUX_UI_TEXT("LuxAnimationTests"));
    LuxUI::CoreApplication::setApplicationName(LUX_UI_TEXT("AnimationWorkspace"));
    LuxUI::TemporaryDir files;
    LuxUI::Settings::setDefaultFormat(LuxUI::Settings::IniFormat);
    LuxUI::Settings::setPath(LuxUI::Settings::IniFormat, LuxUI::Settings::UserScope, files.path());
    LuxEditor::EngineSceneRenderer::SetForceAllowed(false);
    try {
        Check(files.isValid(), "Animation test directory is unavailable");
        LuxEngine::Scene scene;
        auto entity = scene.CreateEntity("Animated");
        auto& animation = entity.AddComponent<LuxEngine::AnimationComponent>();
        animation.clip.loop = false;
        animation.clip.duration = 2;
        animation.clip.AddKeyframe({0, {}, {}, {1, 1, 1}});
        animation.clip.AddKeyframe({1, {5, 1, 0}, {}, {1, 1, 1}});
        animation.clip.AddKeyframe({2, {10, 2, 0}, {}, {1, 1, 1}});
        const auto path = files.filePath(LUX_UI_TEXT("Animation.lux"));
        Check(scene.Serialize(path.toUtf8().toStdString()), "Animation fixture did not save");
        LuxUI::Settings().setValue(LUX_UI_TEXT("lastScene"), path);
        LuxEditor::EditorWindow window;
        window.resize(1600, 1000);
        window.show();
        window.ExecuteRecordedEdit(LUX_UI_TEXT("select"), [&] {
            window.GetDocument().Select(
                window.GetDocument().GetScene().GetEntities().front().GetId());
        });
        app.processEvents();
        auto* dock = window.findChild<LuxUI::DockWidget*>(LUX_UI_TEXT("AnimationWorkspace"));
        Check(dock, "Animation workspace is unavailable");
        auto* openWorkspace =
            window.findChild<LuxUI::Action*>(LUX_UI_TEXT("openAnimationWorkspace"));
        Check(openWorkspace, "Animation menu action is unavailable");
        openWorkspace->trigger();
        auto* documents = window.findChild<LuxUI::TabWidget*>(LUX_UI_TEXT("animationDocuments"));
        Check(documents, "Animation documents are unavailable");
        documents->setCurrentIndex(1);
        app.processEvents();
        auto* workspace = window.findChild<LuxUI::TabBar*>(LUX_UI_TEXT("workspaceTabs"));
        if (!dock->isVisible())
            std::cerr << "Animation state: tab=" << (workspace ? workspace->currentIndex() : -1)
                      << " hidden=" << dock->isHidden() << " ancestorVisible=" << window.isVisible()
                      << " actionChecked=" << dock->toggleViewAction()->isChecked() << '\n';
        Check(dock->isVisible(), "Animation menu action did not open the workspace");
        auto* selectTool = window.findChild<LuxUI::ToolButton*>(LUX_UI_TEXT("animationTool0"));
        auto* moveTool = window.findChild<LuxUI::ToolButton*>(LUX_UI_TEXT("animationTool1"));
        Check(selectTool && moveTool, "Animation tools are unavailable");
        moveTool->click();
        Check(moveTool->isChecked(), "Animation move button did not select its tool");
        selectTool->click();
        window.activateWindow();
        selectTool->setFocus();
        app.processEvents();
        LuxUI::KeyEvent moveKey(LuxUI::Event::KeyPress, LuxUI::Flags::Key_W,
                                LuxUI::Flags::NoModifier);
        LuxUI::Application::sendEvent(selectTool, &moveKey);
        const auto shortcutDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!moveTool->isChecked() && std::chrono::steady_clock::now() < shortcutDeadline) {
            app.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(moveTool->isChecked(), "Animation move shortcut did not select its tool");
        auto* timeline = window.findChild<LuxUI::Slider*>(LUX_UI_TEXT("animationTimeline"));
        auto* keys = window.findChild<LuxUI::ListWidget*>(LUX_UI_TEXT("animationKeys"));
        auto* keyTime = window.findChild<LuxUI::DoubleSpinBox*>(LUX_UI_TEXT("animationKeyTime"));
        auto* keyX = window.findChild<LuxUI::DoubleSpinBox*>(LUX_UI_TEXT("animationKeyValue00"));
        auto* undo = window.findChild<LuxUI::UndoStack*>();
        Check(timeline && keys && keyTime && keyX && undo,
              "Animation editing controls are unavailable");
        Check(window.findChild<LuxUI::Widget*>(LUX_UI_TEXT("animationPreview2D")) &&
                  window.findChild<LuxUI::Widget*>(LUX_UI_TEXT("animationPreview3D")),
              "Dedicated animation previews are unavailable");
        keys->setCurrentRow(1);
        Check(std::abs(keyTime->value() - 1) < .0001, "Key selection did not populate details");
        keyX->setValue(7.5);
        auto current = [&]() -> LuxEngine::AnimationComponent& {
            return window.GetDocument().GetSelected().GetComponent<LuxEngine::AnimationComponent>();
        };
        Check(current().clip.keyframes[1].position.x == 7.5f,
              "Key value edit did not update the clip");
        undo->undo();
        Check(current().clip.keyframes[1].position.x == 5, "Key value undo failed");
        undo->redo();
        auto* autoPlay = window.findChild<LuxUI::CheckBox*>(LUX_UI_TEXT("animationAutoPlay"));
        Check(autoPlay && autoPlay->isChecked(), "Runtime autoplay state is unavailable");
        const auto countBeforePause = current().clip.keyframes.size();
        autoPlay->setChecked(false);
        Check(!current().playing && current().clip.keyframes.size() == countBeforePause &&
                  current().clip.keyframes[1].position.x == 7.5f,
              "Disabling autoplay discarded authored animation keys");
        undo->undo();
        Check(current().playing && current().clip.keyframes.size() == countBeforePause,
              "Autoplay undo did not preserve the clip");
        keys->setCurrentRow(1);
        keyTime->setValue(1.25);
        Check(current().clip.keyframes[1].time == 1.25f, "Key time edit did not update the clip");
        undo->undo();
        Check(current().clip.keyframes[1].time == 1, "Key time undo failed");
        undo->redo();
        keys->setCurrentRow(1);
        timeline->resize(1000, 220);
        const double span = timeline->width() - 174 - 18;
        const LuxUI::PointF start(174 + span * 1.25 / 2, 86);
        const LuxUI::PointF end(174 + span * 1.5 / 2, 86);
        Mouse(timeline, LuxUI::Event::MouseButtonPress, start, LuxUI::Flags::LeftButton,
              LuxUI::Flags::LeftButton);
        Mouse(timeline, LuxUI::Event::MouseMove, end, LuxUI::Flags::NoButton,
              LuxUI::Flags::LeftButton);
        Mouse(timeline, LuxUI::Event::MouseButtonRelease, end, LuxUI::Flags::LeftButton,
              LuxUI::Flags::NoButton);
        Check(std::abs(current().clip.keyframes[1].time - 1.5f) < .0001f,
              "Dragging a key on the dopesheet did not move it");
        undo->undo();
        Check(current().clip.keyframes[1].time == 1.25f, "Key drag undo failed");
        if (const char* capture = std::getenv("LUX_ANIMATION_CAPTURE")) {
            app.processEvents();
            Check(window.grab().save(LuxUI::String::fromUtf8(capture)),
                  "Animation screenshot did not save");
        }
        auto* next = window.findChild<LuxUI::PushButton*>(LUX_UI_TEXT("animationNextFrame"));
        auto* add = window.findChild<LuxUI::PushButton*>(LUX_UI_TEXT("animationAddKey"));
        Check(next && add, "Animation frame transport is unavailable");
        current().clip.duration = 120;
        current().currentTime = 0;
        next->click();
        const float preciseTime = current().currentTime;
        add->click();
        Check(preciseTime > 0 && std::abs(current().clip.keyframes[1].time - preciseTime) < .0001f,
              "Key insertion lost sub-slider frame precision on a long clip");
        const auto keysBeforeDelete = current().clip.keyframes.size();
        const auto entitiesBeforeDelete = window.GetDocument().GetScene().GetEntityCount();
        LuxUI::KeyEvent overrideDelete(LuxUI::Event::ShortcutOverride, LuxUI::Flags::Key_Delete,
                                       LuxUI::Flags::NoModifier);
        LuxUI::KeyEvent deleteKey(LuxUI::Event::KeyPress, LuxUI::Flags::Key_Delete,
                                  LuxUI::Flags::NoModifier);
        LuxUI::Application::sendEvent(timeline, &overrideDelete);
        LuxUI::Application::sendEvent(timeline, &deleteKey);
        Check(overrideDelete.isAccepted() &&
                  current().clip.keyframes.size() + 1 == keysBeforeDelete &&
                  window.GetDocument().GetScene().GetEntityCount() == entitiesBeforeDelete,
              "Timeline Delete removed the scene object instead of its key");
        undo->undo();
        auto* save = window.findChild<LuxUI::PushButton*>(LUX_UI_TEXT("animationSave"));
        Check(save, "Animation save command is unavailable");
        save->click();
        Check(!window.GetDocument().IsDirty(), "Animation save did not persist the clip");
        LuxEngine::Scene persisted;
        Check(persisted.Deserialize(path.toUtf8().toStdString()),
              "Saved animation scene did not reopen");
        Check(persisted.GetEntities()
                      .front()
                      .GetComponent<LuxEngine::AnimationComponent>()
                      .clip.keyframes.size() == current().clip.keyframes.size(),
              "Saved animation keys were lost");
        window.GetDocument().SetDirty(false);
        window.close();
        std::cout << "Animation workspace key editing, drag, undo and frame precision passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
