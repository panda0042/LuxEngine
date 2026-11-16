#include <LuxEditor/Animation/AnimationAssetEditor.hpp>
#include <LuxEditor/Scene/EditorDocument.hpp>
#include <LuxEditor/Viewport/ViewportCamera3D.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/Widgets.hpp>
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
