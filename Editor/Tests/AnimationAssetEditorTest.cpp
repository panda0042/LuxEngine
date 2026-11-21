#include <LuxEditor/Animation/AnimationAssetEditor.hpp>
#include <LuxEditor/Scene/EditorDocument.hpp>
#include <LuxEditor/Viewport/ViewportCamera3D.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    LuxUI::Application application(argc, argv);
    try {
        LuxEditor::EditorDocument source;
        auto entity = source.CreateEntity("Animated");
        entity.GetComponent<LuxEngine::TransformComponent>().position = {1e12 + .125, 0, 0};
        source.Select(entity.GetId());
        LuxEditor::AnimationAssetEditor editor(
            nullptr, source,
            [](const LuxUI::String&, const std::function<void()>& edit) { edit(); });
        editor.Document().Edit([](auto& state) {
            LuxEngine::AnimationTrack track;
            track.SetKey({0, {0, 0, 0, 0}});
            track.SetKey({1, {10, 0, 0, 0}});
            state.clip.tracks.push_back(track);
        });
        editor.Seek(.5);
        if (!editor.PreviewSourceUnchanged() ||
            source.GetScene()
                    .GetComponent<LuxEngine::TransformComponent>(entity.GetId())
                    .position.x != 1e12 + .125)
            throw std::runtime_error("Preview mutated authored scene");
        const auto path =
            (std::filesystem::temp_directory_path() / "lux-editor-clip-test.luxanim").string();
        if (!editor.SaveAsset(path))
            throw std::runtime_error("Clip did not save");
        editor.Document().Edit([](auto& state) { state.clip.duration = 2; });
        if (!editor.Document().Undo() || editor.Document().State().clip.duration != 1)
            throw std::runtime_error("Asset Undo failed");
        if (!editor.Document().Redo() || editor.Document().State().clip.duration != 2)
            throw std::runtime_error("Asset Redo failed");
        if (!editor.OpenAsset(path) || editor.Document().State().clip.duration != 1)
            throw std::runtime_error("Clip did not reopen");
        if (!editor.findChild<LuxUI::Widget*>(LUX_UI_TEXT("animationAssetCurve")) ||
            !editor.findChild<LuxUI::Widget*>(LUX_UI_TEXT("animationAnimatorGraph")))
            throw std::runtime_error("Authoring views missing");
        editor.Document().Edit([](auto& state) { state.clip.loop = false; });
        editor.show();
        LuxUI::Application::processEvents();
        LuxUI::DoubleSpinBox* speed = nullptr;
        const auto controls = editor.findChildren<LuxUI::DoubleSpinBox*>();
        for (auto* form : editor.findChildren<LuxUI::FormLayout*>())
            for (auto* spin : controls)
                if (const auto* label = qobject_cast<LuxUI::Label*>(form->labelForField(spin));
                    label && label->text() == LUX_UI_TEXT("속도"))
                    speed = spin;
        if (!speed)
            throw std::runtime_error("Preview speed control is missing");
        speed->setValue(-1);
        const auto reverseFrom = [&](double start) {
            editor.Seek(start);
            editor.Play(true);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            while (editor.Time() == (start == 0 ? 1 : start) &&
                   std::chrono::steady_clock::now() < deadline) {
                LuxUI::Application::processEvents();
                LuxUI::Thread::msleep(5);
            }
            editor.Play(false);
            return editor.Time();
        };
        const auto reverse = reverseFrom(0);
        if (reverse <= .5 || reverse >= 1)
            throw std::runtime_error("A reverse one-shot preview did not start at the endpoint");
        const auto positioned = reverseFrom(.5);
        if (positioned <= 0 || positioned >= .5)
            throw std::runtime_error("A reverse preview changed the explicit start time");
        auto camera = LuxEditor::MakeViewportCamera3D({800, 600}, {1e12, 0, 0}, 0, 0, 10);
        auto farPoint = camera.GetPosition() +
                        (camera.GetTarget() - camera.GetPosition()).Normalized() * 100000;
        if (!LuxEditor::ProjectViewportPoint(camera, {800, 600}, farPoint))
            throw std::runtime_error("Scene View still clips distant objects");
        std::filesystem::remove(path);
        std::cout << "Animation asset authoring and isolated preview passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
