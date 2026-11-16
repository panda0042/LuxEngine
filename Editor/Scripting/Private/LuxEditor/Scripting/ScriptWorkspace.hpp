#pragma once

#include <LuxEditor/Build/ProjectBuild.hpp>
#include <LuxUI/CodeEditor.hpp>
#include <filesystem>
#include <memory>
#include <vector>

namespace LuxEditor {
class CSharpLanguageClient;
class ScriptDebugger;

class ScriptWorkspace final : public LuxUI::Widget {
public:
    explicit ScriptWorkspace(LuxUI::Widget* parent = nullptr);
    ~ScriptWorkspace() override;
    bool OpenFile(const std::filesystem::path& path, std::string& error, int line = 1,
                  int column = 1);
    bool CreateScript(const std::string& name, std::string& error);
    bool SaveFile(int index, std::string& error);
    bool ReloadFile(int index, std::string& error, bool discard = false);
    bool SaveAll();
    bool ConfirmCloseAll();
    void SetProjectRoot(const std::filesystem::path& root);
    void SetExecutionActive(bool active);
    void BuildScripts();
    bool IsBuilding() const;
    void AddDiagnostic(const BuildDiagnostic& diagnostic,
                       const std::filesystem::path& relativeRoot = {});
    LuxUI::CodeEditor* CurrentEditor() const;
    void AttachDebugger(qint64 processId);
    void StopDebugger();
    bool IsDebugging() const;
    std::function<void()> debugRequested;
    std::function<void()> debugStopped;

private:
    struct Source {
        std::filesystem::path path;
        LuxUI::CodeEditor* editor = nullptr;
        LuxUI::ByteArray disk;
        bool crlf = false;
        bool bom = false;
        int version = 1;
    };

    void CloseFile(int index);
    void RefreshFiles();
    void RefreshOutline();
    void UpdateCaption(LuxUI::CodeEditor* editor);
    void UpdateStatus();
    void ShowFailure(const std::string& error);
    void ReadBuildOutput(bool flush = false);
    void StopBuild();
    void BuildScriptServices(LuxUI::ToolBar* toolbar, LuxUI::TabWidget* bottom);
    void ConfigureScriptTools();
    void RestartLanguage();
    void SourceChanged(LuxUI::CodeEditor* editor);
    void CompleteCode();
    void GoToDefinition();
    void SaveBreakpoints(LuxUI::CodeEditor* editor);
    void OnDebugEvent(const LuxUI::JsonObject& event);
    void ShowDebugFrame(const LuxUI::JsonObject& frame);
    void RefreshDebugControls();
    std::vector<Source> sources_;
    std::filesystem::path root_;
    LuxUI::TabWidget* tabs_ = nullptr;
    LuxUI::TreeWidget* files_ = nullptr;
    LuxUI::TreeWidget* outline_ = nullptr;
    LuxUI::ListWidget* problems_ = nullptr;
    LuxUI::PlainTextEdit* output_ = nullptr;
    LuxUI::Widget* searchBar_ = nullptr;
    LuxUI::LineEdit* find_ = nullptr;
    LuxUI::LineEdit* replace_ = nullptr;
    LuxUI::CheckBox* matchCase_ = nullptr;
    LuxUI::Label* status_ = nullptr;
    LuxUI::Label* filePath_ = nullptr;
    LuxUI::ToolButton* build_ = nullptr;
    LuxUI::ToolButton* create_ = nullptr;
    LuxUI::ToolButton* stop_ = nullptr;
    LuxUI::Process* process_ = nullptr;
    LuxUI::ByteArray buildBuffer_;
    LuxUI::Timer outlineTimer_;
    bool executionActive_ = false;
    bool debugAfterBuild_ = false;
    std::unique_ptr<CSharpLanguageClient> language_;
    std::unique_ptr<ScriptDebugger> debugger_;
    LuxUI::ListWidget* stack_ = nullptr;
    LuxUI::TreeWidget* variables_ = nullptr;
    LuxUI::LineEdit* watch_ = nullptr;
    std::vector<LuxUI::ToolButton*> debugControls_;
    int debugFrame_ = 0;
};
} // namespace LuxEditor
