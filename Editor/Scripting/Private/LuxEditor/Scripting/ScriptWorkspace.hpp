#pragma once

#include <LuxEditor/Build/ProjectBuild.hpp>
#include <LuxUI/CodeEditor.hpp>
#include <filesystem>
#include <vector>

namespace LuxEditor {

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

private:
    struct Source {
        std::filesystem::path path;
        LuxUI::CodeEditor* editor = nullptr;
        LuxUI::ByteArray disk;
        bool crlf = false;
        bool bom = false;
    };

    void CloseFile(int index);
    void RefreshFiles();
    void RefreshOutline();
    void UpdateCaption(LuxUI::CodeEditor* editor);
    void UpdateStatus();
    void ShowFailure(const std::string& error);
    void ReadBuildOutput(bool flush = false);
    void StopBuild();
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
};
} // namespace LuxEditor
