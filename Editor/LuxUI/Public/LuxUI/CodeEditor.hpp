#pragma once

#include <LuxUI/Widgets.hpp>
#include <memory>
#include <vector>

namespace LuxUI {

struct CodeSymbol {
    String name;
    int line = 1;
};

struct CodeTextEdit {
    int line = 1, column = 1, endLine = 1, endColumn = 1;
    String text;
};

struct CodeCompletion {
    String label, detail;
    std::vector<CodeTextEdit> edits;
};

struct CodeBreakpoint {
    int line = 1;
    String condition;
    bool verified = false;
};

// The Qt text document, highlighting and gutter stay inside the LuxUI boundary.
class CodeEditor final : public PlainTextEdit {
    Q_OBJECT
public:
    explicit CodeEditor(Widget* parent = nullptr);
    ~CodeEditor() override;
    void SetCleanText(const String& text);
    void MarkSaved();
    bool IsModified() const;
    int CurrentLine() const;
    int CurrentColumn() const;
    void GoTo(int line, int column = 1);
    bool FindText(const String& text, bool backwards = false, bool matchCase = false);
    int ReplaceAll(const String& text, const String& replacement, bool matchCase = false);
    std::vector<CodeSymbol> Outline() const;
    int GutterWidth() const;
    void PaintGutter(PaintEvent* event);
    void ClickGutter(int y, bool condition);
    void ShowCompletions(const std::vector<CodeCompletion>& items);
    bool ApplyEdits(const std::vector<CodeTextEdit>& edits);
    void ToggleBreakpoint(int line);
    std::vector<CodeBreakpoint> Breakpoints() const;
    void SetBreakpoints(const std::vector<CodeBreakpoint>& breakpoints);
    void SetExecutionLine(int line);
    String CurrentWord() const;

Q_SIGNALS:
    void FindRequested();
    void SaveRequested();
    void CompletionRequested();
    void DefinitionRequested();
    void BreakpointsChanged();
    void BreakpointConditionRequested(int line);

protected:
    void resizeEvent(ResizeEvent* event) override;
    void keyPressEvent(KeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    void UpdateGutter();
    void HighlightLine();
    Widget* gutter_ = nullptr;
    struct Services;
    std::unique_ptr<Services> services_;
};

} // namespace LuxUI
