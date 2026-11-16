#include <LuxEditor/Animation/AnimationAssetEditor.hpp>
#include <LuxEditor/App/EditorWindow.hpp>
#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Project/ProjectArchive.hpp>
#include <LuxEditor/Scene/SceneDependencies.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <LuxEditor/Viewport/SceneViewport2D.hpp>
#include <LuxEditor/Viewport/SceneViewport3D.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxUI/CodeEditor.hpp>
#include <LuxUI/Widgets.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
using namespace LuxEditor;
using namespace LuxEngine;

void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

LuxUI::PushButton* Button(LuxUI::Widget& parent, const char* label) {
    for (auto* b : parent.findChildren<LuxUI::PushButton*>())
        if (b->text() == LuxUI::String::fromUtf8(label))
            return b;
    throw std::runtime_error(label);
}

void Key(LuxUI::Widget* w, int key, LuxUI::Flags::KeyboardModifiers modifiers) {
    LuxUI::KeyEvent e(LuxUI::Event::KeyPress, key, modifiers);
    LuxUI::Application::sendEvent(w, &e);
}

int main(int argc, char** argv) {
    LuxUI::Application app(argc, argv);
    LuxUI::TemporaryDir files;
    LuxUI::CoreApplication::setOrganizationName("LuxEditorAudit");
    LuxUI::CoreApplication::setApplicationName("IsolatedAudit");
    LuxUI::Settings::setDefaultFormat(LuxUI::Settings::IniFormat);
    LuxUI::Settings::setPath(LuxUI::Settings::IniFormat, LuxUI::Settings::UserScope, files.path());
    auto dir = PathFromUtf8(files.path().toUtf8().toStdString());
    std::string mode = argc > 1 ? argv[1] : "menus";
    try {
        if (mode == "asyncplay") {
            Scene next;
            next.CreateEntity("Background");
            next.Serialize(PathUtf8(dir / "background.lux"));
            EditorWindow w;
            w.OpenSceneDocument(dir / "background.lux", true);
            auto active = w.NewSceneDocument();
            w.GetDocument().CreateEntity("Playing another scene");
            w.findChild<LuxUI::Action*>("previewStart")->trigger();
            Check(w.GetRuntimeScene() != nullptr, "runtime_before_background_load");
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (w.PendingSceneLoadCount() && std::chrono::steady_clock::now() < deadline) {
                app.processEvents();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            Check(w.PendingSceneLoadCount() == 0, "background_load_finished");
            Check(w.GetDocument().GetActiveTabId() == active, "active_tab_unchanged");
            Check(w.GetRuntimeScene() != nullptr, "runtime_after_background_load");
            w.GetDocument().SetDirty(false);
            return 0;
        }
        if (mode == "utf8tail") {
            for (auto tail : {std::string("\xc3", 1), std::string("\xe2\x82", 2),
                              std::string("\xf0\x9f\x92", 3)}) {
                auto file = dir / ("tail" + std::to_string(tail.size()) + ".cs");
                {
                    std::ofstream stream(file, std::ios::binary);
                    stream << "class A {}" << tail;
                }
                ScriptWorkspace workspace;
                std::string error;
                const bool opened = workspace.OpenFile(file, error);
                std::cout << "incomplete_utf8_" << tail.size() << "_accepted=" << opened
                          << std::endl;
                Check(!opened, "incomplete UTF-8 source was accepted");
                if (opened) {
                    workspace.CurrentEditor()->insertPlainText(" ");
                    workspace.SaveFile(0, error);
                    std::ifstream stream(file, std::ios::binary);
                    std::string bytes{std::istreambuf_iterator<char>(stream), {}};
                    Check(bytes.ends_with(tail), "original_tail_preserved");
                }
            }
            return 0;
        }
        if (mode == "cloneassets") {
            std::string error;
            auto project =
                EditorProject::Create(dir / "Original", "Original", ProjectTemplate::Empty, error);
            if (!project)
                throw std::runtime_error(error);
            auto originalTexture = project->GetSceneFile().parent_path() / "sheet.png";
            LuxUI::Image image(2, 2, LuxUI::Image::Format_ARGB32);
            image.fill(0xffffffff);
            image.save(LuxUI::String::fromUtf8(PathUtf8(originalTexture)));
            Scene scene;
            auto entity = scene.CreateEntity("Sprite");
            entity.AddComponent<SpriteRendererComponent>().texturePath = "sheet.png";
            scene.Serialize(PathUtf8(project->GetSceneFile()));
            EditorDocument author;
            author.Open(PathUtf8(project->GetSceneFile()));
            author.Save();
            auto clone = CloneProject(*project, dir / "Clone", "Clone", error);
            if (!clone)
                throw std::runtime_error(error);
            Scene loaded;
            if (!loaded.Deserialize(PathUtf8(clone->GetSceneFile())))
                throw std::runtime_error("clone scene");
            auto reference = PathFromUtf8(loaded.ResolveAssetPath(
                loaded.GetComponent<SpriteRendererComponent>(1).texturePath));
            Check(std::filesystem::exists(clone->GetSceneFile().parent_path() / "sheet.png"),
                  "cloned_asset_file_exists");
            Check(!(reference == originalTexture), "clone_still_references_original");
            scene.GetComponent<SpriteRendererComponent>(1).texturePath = PathUtf8(originalTexture);
            Check(scene.Serialize(PathUtf8(project->GetSceneFile())),
                  "legacy absolute scene fixture failed");
            auto legacy = CloneProject(*project, dir / "LegacyClone", "LegacyClone", error);
            Check(legacy.has_value(), error.c_str());
            Check(loaded.Deserialize(PathUtf8(legacy->GetSceneFile())),
                  "legacy clone failed to load");
            reference = PathFromUtf8(loaded.ResolveAssetPath(
                loaded.GetComponent<SpriteRendererComponent>(1).texturePath));
            Check(reference != originalTexture && std::filesystem::is_regular_file(reference),
                  "legacy absolute asset path was not relocated during export");
            return 0;
        }
        if (mode == "dependencies") {
            std::string error;
            auto project =
                EditorProject::Create(dir / "Project", "Audit", ProjectTemplate::Empty, error);
            Check(project.has_value(), error.c_str());
            const auto assets = dir / "Project" / "Assets";
            const auto imagePath = PathUtf8(assets / "sheet.png");
            LuxUI::Image image(2, 2, LuxUI::Image::Format_ARGB32);
            image.fill(0xffffffff);
            Check(image.save(LuxUI::String::fromUtf8(imagePath)), "image fixture failed");
            AnimationClip clip;
            clip.name = "Walk";
            clip.spriteSheet = imagePath;
            const auto clipPath = PathUtf8(assets / "walk.luxanim");
            Check(clip.Save(clipPath, error), error.c_str());
            AnimatorController controller;
            controller.layers.emplace_back();
            controller.layers[0].entry = "Walk";
            controller.layers[0].states.push_back({"Walk", clipPath});
            const auto controllerPath = PathUtf8(assets / "walk.luxanimator");
            Check(controller.Save(controllerPath, error), error.c_str());
            Scene scene;
            auto entity = scene.CreateEntity("Resources");
            entity.AddComponent<SpriteRendererComponent>().texturePath = imagePath;
            entity.AddComponent<AnimatorComponent>().controllerPath = controllerPath;
            entity.AddComponent<AnimationComponent>().assetPath = clipPath;
            Check(ValidateProjectSceneReferences(*project, scene, error), error.c_str());
            Check(scene.Serialize(PathUtf8(project->GetSceneFile())), "scene fixture failed");
            Check(ValidateProjectScenes(*project, error), error.c_str());
            for (auto* resource : {&entity.GetComponent<SpriteRendererComponent>().texturePath,
                                   &entity.GetComponent<AnimatorComponent>().controllerPath,
                                   &entity.GetComponent<AnimationComponent>().assetPath}) {
                const auto original = *resource;
                *resource = "does-not-exist.asset";
                Check(!ValidateProjectSceneReferences(*project, scene, error),
                      "missing resource accepted");
                *resource = original;
            }
            clip.spriteSheet = "missing-sheet.png";
            Check(clip.Save(clipPath, error), error.c_str());
            Check(!ValidateProjectScenes(*project, error), "missing transitive image accepted");
            clip.spriteSheet = imagePath;
            Check(clip.Save(clipPath, error), error.c_str());
            Check(ValidateProjectScenes(*project, error), error.c_str());
            return 0;
        }
        if (mode == "layercrash") {
            EditorDocument doc;
            AnimationAssetEditor ed(nullptr, doc, [](auto&, auto& f) { f(); });
            ed.Document().New(true);
            Button(ed, "레이어 추가")->click();
            std::cout << "layers_before_undo=" << ed.Document().State().controller.layers.size()
                      << std::endl;
            Key(&ed, LuxUI::Flags::Key_Z, LuxUI::Flags::ControlModifier);
            Check(ed.Document().State().controller.layers.size() == 1,
                  "undo did not remove the layer");
            Button(ed, "선택 상태 삭제")->click();
            std::cout << "delete_survived" << std::endl;
            return 0;
        }
        if (mode == "events") {
            EditorDocument doc;
            AnimationAssetEditor ed(nullptr, doc, [](auto&, auto& f) { f(); });
            auto page = Button(ed, "선택 이벤트 삭제")->parentWidget();
            auto* list = page->findChild<LuxUI::ListWidget*>();
            ed.Document().Edit([](auto& s) { s.clip.events.push_back({.2, "Existing", ""}); });
            auto path = PathUtf8(dir / "events.luxanim");
            ed.SaveAsset(path);
            ed.OpenAsset(path);
            Check(ed.Document().State().clip.events.size() == 1 && list->count() == 1,
                  "loaded events were not displayed");
            auto inputs = page->findChildren<LuxUI::LineEdit*>();
            inputs.front()->setText("Earlier");
            ed.findChild<LuxUI::DoubleSpinBox*>("animationAssetTime")->setValue(.1);
            Button(ed, "현재 시간에 이벤트 추가")->click();
            Key(&ed, LuxUI::Flags::Key_Z, LuxUI::Flags::ControlModifier);
            Check(ed.Document().State().clip.events.size() == 1 && list->count() == 1 &&
                      list->item(0)->text().contains("Existing"),
                  "undo left a stale event row");
            list->setCurrentRow(0);
            std::cout << "clicked_visible_event=" << list->currentItem()->text().toStdString()
                      << std::endl;
            Button(ed, "선택 이벤트 삭제")->click();
            Check(ed.Document().State().clip.events.empty(), "selected event was not deleted");
            ed.UndoEdit();
            Check(ed.Document().State().clip.events.size() == 1 && list->count() == 1,
                  "event delete undo failed");
            return 0;
        }
        if (mode == "copy") {
            EditorWindow w;
            auto& d = w.GetDocument();
            auto entity = d.CreateEntity("Textured animated sprite");
            auto& sprite = entity.AddComponent<SpriteRendererComponent>();
            sprite.texturePath = "sheet.png";
            sprite.frame = 5;
            sprite.columns = 4;
            sprite.rows = 4;
            entity.AddComponent<AnimatorComponent>().controllerPath = "controller.luxanimator";

            auto& camera = entity.AddComponent<CameraComponent>();
            camera.nearClip = .25;
            camera.farClip = 2500;
            d.Select(entity.GetId());
            w.findChild<LuxUI::Action*>("copyEntity")->trigger();
            w.findChild<LuxUI::Action*>("pasteEntity")->trigger();
            auto copy = d.GetSelected();
            Check(copy.GetId() != entity.GetId(), "new_entity");
            Check(d.GetScene().ResolveAssetPath(
                      copy.GetComponent<SpriteRendererComponent>().texturePath) ==
                      d.GetScene().ResolveAssetPath("sheet.png"),
                  "sprite_texture_preserved");
            Check(copy.GetComponent<SpriteRendererComponent>().frame == 5,
                  "sprite_frame_preserved");
            Check(copy.HasComponent<AnimatorComponent>(), "animator_preserved");

            Check(copy.GetComponent<CameraComponent>().nearClip == .25 &&
                      copy.GetComponent<CameraComponent>().farClip == 2500,
                  "camera_clips_preserved");
            d.SetDirty(false);
            return 0;
        }
        EditorDocument d;
        auto e = d.CreateEntity("Target");
        d.Select(e.GetId());
        if (mode == "crash") {
            AnimationAssetEditor ed(nullptr, d, [](auto&, auto& f) { f(); });
            Button(ed, "트랙 추가")->click();
            Button(ed, "현재 시간에 키 추가")->click();
            std::cout << "keys_before_undo=" << ed.Document().State().clip.tracks[0].keys.size()
                      << std::endl;
            ed.findChild<LuxUI::ListWidget*>("animationAssetKeys")->setCurrentRow(0);
            Key(&ed, LuxUI::Flags::Key_Z, LuxUI::Flags::ControlModifier);
            Check(ed.Document().State().clip.tracks[0].keys.empty(), "undo did not remove the key");
            auto* curve = ed.findChild<LuxUI::Widget*>("animationAssetCurve");
            Key(curve, LuxUI::Flags::Key_C, LuxUI::Flags::ControlModifier);
            ed.RedoEdit();
            curve->resize(640, 240);
            LuxUI::MouseEvent press(LuxUI::Event::MouseButtonPress, {56, 123.5}, {56, 123.5},
                                    LuxUI::Flags::LeftButton, LuxUI::Flags::LeftButton,
                                    LuxUI::Flags::NoModifier);
            LuxUI::Application::sendEvent(curve, &press);
            ed.UndoEdit();
            LuxUI::MouseEvent move(LuxUI::Event::MouseMove, {100, 110}, {100, 110},
                                   LuxUI::Flags::NoButton, LuxUI::Flags::LeftButton,
                                   LuxUI::Flags::NoModifier);
            LuxUI::Application::sendEvent(curve, &move);
            Check(ed.Document().State().clip.tracks[0].keys.empty(),
                  "undo left an active key drag");
            std::cout << "copy_survived" << std::endl;
            return 0;
        }
        if (mode == "precision") {
            const double original = 1234567890123.125;
            qputenv("SDL_VIDEODRIVER", "dummy");
            EditorWindow window;
            auto* canvas =
                dynamic_cast<SceneViewport2D*>(window.findChild<LuxUI::Widget*>("sceneViewport2D"));
            Check(canvas && canvas->SetView(original, -original, 10), "large view setup failed");
            window.findChild<LuxUI::Action*>("playWindow")->trigger();
            bool checked = false;
            for (auto* process : window.findChildren<LuxUI::Process*>()) {
                const auto args = process->arguments();
                const auto index = args.indexOf("--view");
                if (index >= 0) {
                    Check(args[index + 1].toDouble() == original &&
                              args[index + 2].toDouble() == -original,
                          "player view coordinates lost precision");
                    checked = true;
                }
            }
            Check(checked, "player view arguments were not produced");
            window.findChild<LuxUI::Action*>("playWindowStop")->trigger();
            return 0;
        }
        if (mode == "preview") {
            e.GetComponent<TransformComponent>().position.x = 10;
            e.AddComponent<MeshRendererComponent>();
            AnimationAssetEditor ed(nullptr, d, [](auto&, auto& f) { f(); });
            ed.Seek(0);
            SceneViewport3D* view = nullptr;
            for (auto* widget : ed.findChildren<LuxUI::Widget*>())
                if (auto* v = dynamic_cast<SceneViewport3D*>(widget))
                    view = v;
            view->FocusSelection();
            auto distance = [&] {
                auto c = view->GetViewCamera();
                auto delta = c.GetPosition() - c.GetTarget();
                return std::sqrt(delta.Dot(delta));
            };
            std::cout << "preview_distance_before=" << distance() << std::endl;
            Scene next;
            auto n = next.CreateEntity("Different scene");
            n.GetComponent<TransformComponent>().position.x = 100;
            n.AddComponent<MeshRendererComponent>();
            n.GetComponent<TransformComponent>().scale = {10, 10, 10};
            next.Serialize(PathUtf8(dir / "next.lux"));
            d.Open(PathUtf8(dir / "next.lux"));
            d.Select(1);
            ed.Seek(0);
            view->FocusSelection();
            std::cout << "source_after="
                      << d.GetSelected().GetComponent<TransformComponent>().position.x << std::endl;
            Check(distance() > 20, "preview kept the preceding scene geometry");
            return 0;
        }
        if (mode == "external") {
            AnimationAssetDocument local;
            std::string err;
            auto path = PathUtf8(dir / "external.luxanim");
            local.Edit([](auto& s) { s.clip.duration = 2; });
            local.Save(path, err);
            AnimationClip other;
            other.name = "External";
            other.duration = 7;
            other.Save(path, err);
            local.Edit([](auto& s) { s.clip.duration = 3; });
            Check(!(local.Save(path, err)), "stale_save_accepted");
            auto onDisk = AnimationClip::Load(path, err);
            Check(onDisk && onDisk->duration == 7, "external edit was overwritten");
            Check(local.Dirty(), "rejected save cleared dirty state");
            Check(local.Save(PathUtf8(dir / "saved-as.luxanim"), err),
                  "saving the conflict under another name failed");
            return 0;
        }
        if (mode == "menus") {
            Scene scene;
            scene.CreateEntity("Saved");
            scene.Serialize(PathUtf8(dir / "scene.lux"));
            EditorWindow w;
            w.OpenSceneDocument(dir / "scene.lux");
            w.show();
            auto* ed = dynamic_cast<AnimationAssetEditor*>(
                w.findChild<LuxUI::Widget*>("animationAssetEditor"));
            auto path = PathUtf8(dir / "asset.luxanim");
            ed->SaveAsset(path);
            ed->Document().Edit([](auto& s) { s.clip.duration = 2; });
            w.findChild<LuxUI::Action*>("openAnimationWorkspace")->trigger();
            w.findChild<LuxUI::Action*>("saveScene")->trigger();
            std::string err;
            auto disk = AnimationClip::Load(path, err);
            Check(disk->duration == 2, "menu_save_saved_asset");
            Check(!(ed->Document().Dirty()), "asset_still_dirty");
            Check(w.findChild<LuxUI::Action*>("undoEdit")->isEnabled(), "menu_undo_enabled");
            w.findChild<LuxUI::Action*>("undoEdit")->trigger();
            Check(ed->Document().State().clip.duration == 1, "menu undo targeted the scene");
            w.findChild<LuxUI::Action*>("redoEdit")->trigger();
            Check(ed->Document().State().clip.duration == 2, "menu redo targeted the scene");
            ed->Document().New(false);
            w.GetDocument().SetDirty(false);
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
