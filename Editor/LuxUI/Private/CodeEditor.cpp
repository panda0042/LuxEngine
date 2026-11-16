#include <LuxUI/CodeEditor.hpp>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>

namespace LuxUI {
namespace {
class CSharpHighlighter final : public QSyntaxHighlighter {
public:
    explicit CSharpHighlighter(QTextDocument* document) : QSyntaxHighlighter(document) {}

protected:
    void highlightBlock(const QString& text) override {
        static const QRegularExpression words(QStringLiteral(
            R"(\b(abstract|as|async|await|base|bool|break|case|catch|char|class|const|continue|decimal|default|delegate|do|double|else|enum|event|explicit|false|finally|float|for|foreach|if|implicit|in|int|interface|internal|is|lock|long|namespace|new|null|object|out|override|params|partial|private|protected|public|readonly|ref|return|sealed|short|static|string|struct|switch|this|throw|true|try|typeof|uint|ulong|using|var|virtual|void|while|yield)\b)"));
        static const QRegularExpression numbers(QStringLiteral(R"(\b\d+(?:\.\d+)?[fFdDmM]?\b)"));
        auto matches = words.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), QColor("#71aee8"));
        }
        matches = numbers.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            setFormat(match.capturedStart(), match.capturedLength(), QColor("#b5cea8"));
        }
        int state = previousBlockState() < 0 ? 0 : previousBlockState();
        int i = 0;
        while (i < text.size()) {
            const int start = i;
            if (state == 1 || text.mid(i, 2) == QStringLiteral("/*")) {
                const int end = text.indexOf(QStringLiteral("*/"), i + (state == 1 ? 0 : 2));
                i = end < 0 ? text.size() : end + 2;
                state = end < 0 ? 1 : 0;
                setFormat(start, i - start, QColor("#7a9d6d"));
            } else if (state == 2 || text.mid(i, 2) == QStringLiteral("@\"")) {
                i += state == 2 ? 0 : 2;
                state = 2;
                while (i < text.size()) {
                    if (text[i++] == u'"') {
                        if (i < text.size() && text[i] == u'"')
                            ++i;
                        else {
                            state = 0;
                            break;
                        }
                    }
                }
                setFormat(start, i - start, QColor("#d6aa88"));
            } else if (text.mid(i, 2) == QStringLiteral("//")) {
                setFormat(i, text.size() - i, QColor("#7a9d6d"));
                break;
            } else if (text[i] == u'"' || text[i] == u'\'') {
                const auto quote = text[i++];
                while (i < text.size()) {
                    if (text[i] == u'\\')
                        i = std::min(i + 2, static_cast<int>(text.size()));
                    else if (text[i++] == quote)
                        break;
                }
                setFormat(start, i - start, QColor("#d6aa88"));
            } else
                ++i;
        }
        setCurrentBlockState(state);
    }
};

class CodeGutter final : public QWidget {
public:
    explicit CodeGutter(CodeEditor* editor) : QWidget(editor), editor_(editor) {}

protected:
    void paintEvent(QPaintEvent* event) override {
        editor_->PaintGutter(event);
    }

private:
    CodeEditor* editor_;
};
} // namespace

CodeEditor::CodeEditor(Widget* parent) : PlainTextEdit(parent), gutter_(new CodeGutter(this)) {
    setObjectName(QStringLiteral("scriptCodeEditor"));
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setTabStopDistance(fontMetrics().horizontalAdvance(u' ') * 4);
    new CSharpHighlighter(document());
    connect(this, &QPlainTextEdit::blockCountChanged, this, [this] { UpdateGutter(); });
    connect(this, &QPlainTextEdit::updateRequest, this, [this](const QRect& area, int dy) {
        if (dy)
            gutter_->scroll(0, dy);
        else
            gutter_->update(0, area.y(), gutter_->width(), area.height());
    });
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &CodeEditor::HighlightLine);
    UpdateGutter();
    HighlightLine();
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::SetCleanText(const String& text) {
    setPlainText(text);
    document()->setModified(false);
}

void CodeEditor::MarkSaved() {
    document()->setModified(false);
}

bool CodeEditor::IsModified() const {
    return document()->isModified();
}

int CodeEditor::CurrentLine() const {
    return textCursor().blockNumber() + 1;
}

int CodeEditor::CurrentColumn() const {
    return textCursor().positionInBlock() + 1;
}

int CodeEditor::GutterWidth() const {
    return 18 + fontMetrics().horizontalAdvance(u'9') * QString::number(blockCount()).size();
}

void CodeEditor::UpdateGutter() {
    setViewportMargins(GutterWidth(), 0, 0, 0);
    const auto area = contentsRect();
    gutter_->setGeometry(area.left(), area.top(), GutterWidth(), area.height());
    gutter_->update();
}

void CodeEditor::resizeEvent(ResizeEvent* event) {
    PlainTextEdit::resizeEvent(event);
    UpdateGutter();
}

void CodeEditor::PaintGutter(PaintEvent* event) {
    QPainter painter(gutter_);
    painter.fillRect(event->rect(), palette().color(QPalette::AlternateBase));
    auto block = firstVisibleBlock();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    while (block.isValid() && top <= event->rect().bottom()) {
        const int height = qRound(blockBoundingRect(block).height());
        if (block.isVisible() && top + height >= event->rect().top()) {
            painter.setPen(block.blockNumber() == textCursor().blockNumber()
                               ? palette().color(QPalette::Text)
                               : palette().color(QPalette::PlaceholderText));
            painter.drawText(0, top, gutter_->width() - 8, fontMetrics().height(), Qt::AlignRight,
                             QString::number(block.blockNumber() + 1));
        }
        block = block.next();
        top += height;
    }
}

void CodeEditor::HighlightLine() {
    QTextEdit::ExtraSelection selection;
    selection.format.setBackground(palette().color(QPalette::AlternateBase));
    selection.format.setProperty(QTextFormat::FullWidthSelection, true);
    selection.cursor = textCursor();
    selection.cursor.clearSelection();
    setExtraSelections({selection});
    gutter_->update();
}

void CodeEditor::GoTo(int line, int column) {
    const auto block = document()->findBlockByNumber(std::clamp(line - 1, 0, blockCount() - 1));
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + std::clamp(column - 1, 0, block.length() - 1));
    setTextCursor(cursor);
    centerCursor();
    setFocus();
}

bool CodeEditor::FindText(const String& text, bool backwards, bool matchCase) {
    if (text.isEmpty())
        return false;
    QTextDocument::FindFlags flags;
    if (backwards)
        flags |= QTextDocument::FindBackward;
    if (matchCase)
        flags |= QTextDocument::FindCaseSensitively;
    auto found = document()->find(text, textCursor(), flags);
    if (found.isNull())
        found = document()->find(text, backwards ? toPlainText().size() : 0, flags);
    if (found.isNull())
        return false;
    setTextCursor(found);
    ensureCursorVisible();
    return true;
}

int CodeEditor::ReplaceAll(const String& text, const String& replacement, bool matchCase) {
    if (text.isEmpty())
        return 0;
    auto edit = textCursor();
    edit.beginEditBlock();
    QTextCursor next(document());
    int count = 0;
    while (true) {
        next = document()->find(text, next,
                                matchCase ? QTextDocument::FindCaseSensitively
                                          : QTextDocument::FindFlags{});
        if (next.isNull())
            break;
        next.insertText(replacement);
        ++count;
    }
    edit.endEditBlock();
    return count;
}

std::vector<CodeSymbol> CodeEditor::Outline() const {
    // Lexical navigation only. Semantic completion/rename/debugging require a C# language server.
    static const QRegularExpression declaration(QStringLiteral(
        R"(^\s*(?:(?:public|private|protected|internal|static|sealed|abstract|override|virtual|async|partial|readonly)\s+)*(?:(?:class|struct|interface|enum)\s+(@?\w+)|[\w<>\[\].?,]+\s+(@?\w+)\s*\([^;]*\)\s*(?:\{|$)))"));
    std::vector<CodeSymbol> result;
    for (auto block = document()->begin(); block.isValid(); block = block.next()) {
        const auto match = declaration.match(block.text());
        if (match.hasMatch())
            result.push_back(
                {match.captured(1).isEmpty() ? match.captured(2) + "()" : match.captured(1),
                 block.blockNumber() + 1});
    }
    return result;
}

bool CodeEditor::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        auto* key = static_cast<QKeyEvent*>(event);
        // Text editing owns keys here; W/E/R/Delete must never manipulate scene entities.
        const bool command = key->matches(QKeySequence::Save) || key->matches(QKeySequence::Find);
        if (command || key->matches(QKeySequence::Undo) || key->matches(QKeySequence::Redo) ||
            key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Paste) ||
            key->matches(QKeySequence::Cut) || key->matches(QKeySequence::SelectAll) ||
            (!(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
             (!key->text().isEmpty() || key->key() == Qt::Key_Delete ||
              key->key() == Qt::Key_Backspace))) {
            event->accept();
            return true;
        }
    }
    return PlainTextEdit::event(event);
}

void CodeEditor::keyPressEvent(KeyEvent* event) {
    if (event->matches(QKeySequence::Save)) {
        Q_EMIT SaveRequested();
        return;
    }
    if (event->matches(QKeySequence::Find)) {
        Q_EMIT FindRequested();
        return;
    }
    auto cursor = textCursor();
    if (event->key() == Qt::Key_Tab && !cursor.hasSelection()) {
        cursor.insertText(QString(4 - cursor.positionInBlock() % 4, u' '));
        return;
    }
    if (event->key() == Qt::Key_Return && !cursor.hasSelection()) {
        const auto prefix = cursor.block().text().left(cursor.positionInBlock());
        int indent = 0;
        while (indent < prefix.size() && prefix[indent].isSpace())
            ++indent;
        auto padding = prefix.left(indent);
        if (prefix.trimmed().endsWith(u'{'))
            padding += QStringLiteral("    ");
        cursor.insertText(u'\n' + padding);
        setTextCursor(cursor);
        return;
    }
    PlainTextEdit::keyPressEvent(event);
}
} // namespace LuxUI
