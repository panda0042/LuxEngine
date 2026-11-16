#pragma once

#include <LuxUI/Widgets.hpp>
#include <vector>

namespace LuxUI {

struct CodeSymbol {
    String name;
    int line = 1;
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

Q_SIGNALS:
    void FindRequested();
    void SaveRequested();

protected:
    void resizeEvent(ResizeEvent* event) override;
    void keyPressEvent(KeyEvent* event) override;
    bool event(QEvent* event) override;

private:
    void UpdateGutter();
    void HighlightLine();
    Widget* gutter_ = nullptr;
};

} // namespace LuxUI
