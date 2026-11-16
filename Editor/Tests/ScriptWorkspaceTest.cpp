#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <LuxUI/CodeEditor.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary);
    file << text;
    Check(static_cast<bool>(file), "Source fixture write failed");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
} // namespace

int main(int argc, char** argv) {
    LuxUI::Application app(argc, argv);
    try {
        LuxUI::TemporaryDir temporary;
        Check(temporary.isValid(), "Temporary directory unavailable");
        const auto root =
            LuxEditor::PathFromUtf8(temporary.path().toUtf8().toStdString()) / "한글 Project";
        std::string error;
        const auto project = LuxEditor::EditorProject::Create(
            root, "ScriptWorkspace", LuxEditor::ProjectTemplate::Empty, error);
        Check(project.has_value(), error.c_str());
        LuxEditor::ScriptWorkspace workspace;
        workspace.SetProjectRoot(root);
        workspace.resize(1200, 800);
        workspace.show();
        const auto source = root / "Scripts/Mover.cs";
        Write(source, "\xef\xbb\xbfusing LuxEngine;\r\n\r\npublic class Mover : EntityScript {\r\n"
                      "    protected override void OnUpdate(double deltaTime) {\r\n    }\r\n}\r\n");
        Check(workspace.OpenFile(source, error, 4, 5), error.c_str());
        auto* editor = workspace.CurrentEditor();
        Check(editor && editor->CurrentLine() == 4 && editor->CurrentColumn() == 5,
              "Source line/column navigation failed");
        Check(editor->Outline().size() == 2, "Class and method outline missing");
        Check(editor->GutterWidth() > 20, "Line number gutter unavailable");
        Check(editor->FindText(LUX_UI_TEXT("OnUpdate")), "Find failed");
        Check(editor->ReplaceAll(LUX_UI_TEXT("deltaTime"), LUX_UI_TEXT("elapsed")) == 1,
              "Replace failed");
        editor->undo();
        Check(editor->toPlainText().contains(LUX_UI_TEXT("deltaTime")), "Replace undo failed");
        editor->redo();
        Check(editor->IsModified(), "Dirty state missing");
        Check(workspace.SaveFile(0, error), error.c_str());
        const auto saved = Read(source);
        Check(saved.starts_with("\xef\xbb\xbf") && saved.find("\r\n") != std::string::npos &&
                  saved.find("elapsed") != std::string::npos && !editor->IsModified(),
              "UTF-8 BOM, CRLF or clean state was lost");
        editor->insertPlainText(LUX_UI_TEXT("// local edit\n"));
        Write(source, "public class External {}\n");
        Check(!workspace.SaveFile(0, error) && Read(source) == "public class External {}\n",
              "Save overwrote external changes");
        Check(!workspace.ReloadFile(0, error), "Reload silently discarded unsaved changes");
        Check(workspace.ReloadFile(0, error, true), error.c_str());
        Check(workspace.OpenFile(source, error), error.c_str());
        auto* tabs = workspace.findChild<LuxUI::TabWidget*>(LUX_UI_TEXT("scriptDocuments"));
        Check(tabs->count() == 1, "Opening an existing source duplicated its tab");
        Write(root / "Scripts/Invalid.cs", std::string("\xff\xfe\0", 3));
        Check(!workspace.OpenFile(root / "Scripts/Invalid.cs", error),
              "Invalid UTF-8 was accepted");
        std::filesystem::remove(root / "Scripts/Invalid.cs");
        Check(workspace.CreateScript("Follower", error), error.c_str());
        Check(!workspace.CreateScript("Follower", error), "New script overwrote an existing file");
        Check(!workspace.CreateScript("class", error), "Reserved C# keyword was accepted");
        Check(!workspace.CreateScript("../Escape", error), "Invalid class name was accepted");
        editor = workspace.CurrentEditor();
        bool findRequested = false, saveRequested = false;
        LuxUI::Object::connect(editor, &LuxUI::CodeEditor::FindRequested,
                               [&] { findRequested = true; });
        LuxUI::Object::connect(editor, &LuxUI::CodeEditor::SaveRequested,
                               [&] { saveRequested = true; });
        for (const auto key : {LuxUI::Flags::Key_F, LuxUI::Flags::Key_S}) {
            LuxUI::KeyEvent override(LuxUI::Event::ShortcutOverride, key,
                                     LuxUI::Flags::ControlModifier);
            override.ignore();
            app.sendEvent(editor, &override);
            Check(override.isAccepted(), "Code editor did not own text command shortcut");
            LuxUI::KeyEvent press(LuxUI::Event::KeyPress, key, LuxUI::Flags::ControlModifier);
            app.sendEvent(editor, &press);
        }
        Check(findRequested && saveRequested, "Code search/save shortcuts were not routed");
        workspace.SetExecutionActive(true);
        Check(!workspace.findChild<LuxUI::ToolButton*>(LUX_UI_TEXT("buildScripts"))->isEnabled(),
              "Script build was allowed during PIE");
        workspace.SetExecutionActive(false);
        workspace.BuildScripts();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (workspace.IsBuilding() && std::chrono::steady_clock::now() < deadline) {
            app.processEvents();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        Check(!workspace.IsBuilding(), "Script build did not finish");
        auto* output = workspace.findChild<LuxUI::PlainTextEdit*>(LUX_UI_TEXT("scriptBuildOutput"));
        if (!std::filesystem::is_regular_file(root / ".lux/managed/GameScripts.dll"))
            throw std::runtime_error(output->toPlainText().toUtf8().toStdString());
        const auto diagnostic =
            LuxEditor::ParseBuildLine(LuxEditor::PathUtf8(root / "Scripts/Follower.cs") +
                                      "(6,5): error CS1002: ; expected [GameScripts.csproj]");
        Check(diagnostic.has_value(), "C# compiler diagnostic was not parsed");
        workspace.AddDiagnostic(*diagnostic);
        auto* problems = workspace.findChild<LuxUI::ListWidget*>(LUX_UI_TEXT("scriptProblems"));
        Check(problems->count() == 1, "Compiler diagnostic was not listed");
        problems->itemActivated(problems->item(0));
        Check(workspace.CurrentEditor()->CurrentLine() == 6 &&
                  workspace.CurrentEditor()->CurrentColumn() == 5,
              "Diagnostic did not navigate to the source position");
        const auto capture = qEnvironmentVariable("LUX_SCRIPT_CAPTURE");
        if (!capture.isEmpty()) {
            app.processEvents();
            Check(workspace.grab().save(capture), "Script workspace capture failed");
        }
        std::cout << "Script editing, file conflicts, UTF-8/CRLF, shortcuts, build and diagnostics "
                     "passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
