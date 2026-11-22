#include <LuxUI/CodeEditor.hpp>
#include <QAbstractItemView>
#include <QCompleter>
#include <QFontDatabase>
#include <QRegularExpression>
#include <QStandardItemModel>
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

    void mousePressEvent(QMouseEvent* event) override {
        editor_->ClickGutter(qRound(event->position().y()), event->button() == Qt::RightButton);
    }

private:
    CodeEditor* editor_;
};
} // namespace

struct CodeEditor::Services {
    struct Marker {
        QTextCursor cursor;
        String condition;
        bool verified = false;
    };

    std::vector<Marker> markers;
    int executionLine = 0;
    QCompleter* completer = nullptr;
    std::vector<CodeCompletion> completions;
    int completionRevision = 0;
    QTimer completionTimer;
};

CodeEditor::CodeEditor(Widget* parent)
    : PlainTextEdit(parent), gutter_(new CodeGutter(this)),
      services_(std::make_unique<Services>()) {
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
    services_->completer = new QCompleter(this);
    services_->completer->setWidget(this);
    services_->completer->setCaseSensitivity(Qt::CaseInsensitive);
    services_->completer->setCompletionMode(QCompleter::PopupCompletion);
    connect(services_->completer, qOverload<const QModelIndex&>(&QCompleter::activated), this,
            [this](const QModelIndex& index) {
                const int selected = index.data(Qt::UserRole).toInt();
                if (document()->revision() == services_->completionRevision && selected >= 0 &&
                    size_t(selected) < services_->completions.size())
                    ApplyEdits(services_->completions[selected].edits);
            });
    services_->completionTimer.setSingleShot(true);
    services_->completionTimer.setInterval(180);
    connect(&services_->completionTimer, &QTimer::timeout, this, [this] {
        if (hasFocus())
            Q_EMIT CompletionRequested();
    });
    connect(this, &QPlainTextEdit::textChanged, this, [this] {
        services_->completer->popup()->hide();
        if (!services_->markers.empty())
            Q_EMIT BreakpointsChanged();
    });
}

CodeEditor::~CodeEditor() = default;

void CodeEditor::SetCleanText(const String& text) {
    const auto points = Breakpoints();
    {
        const QSignalBlocker blocker(this);
        setPlainText(text);
        document()->setModified(false);
        SetBreakpoints(points);
    }
    Q_EMIT textChanged();
    Q_EMIT modificationChanged(false);
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
    return 32 + fontMetrics().horizontalAdvance(u'9') * QString::number(blockCount()).size();
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
            for (const auto& marker : services_->markers)
                if (marker.cursor.blockNumber() == block.blockNumber()) {
                    painter.setPen(QColor("#ec6868"));
                    painter.setBrush(marker.verified ? QBrush(QColor("#ec6868"))
                                                     : QBrush(Qt::NoBrush));
                    painter.drawEllipse(QPointF(8, top + height * .5), 5, 5);
                }
            if (services_->executionLine == block.blockNumber() + 1) {
                painter.setPen(QColor("#eecb68"));
                painter.drawText(15, top, 12, fontMetrics().height(), Qt::AlignCenter,
                                 QStringLiteral("▶"));
            }
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
    if (isReadOnly() || text.isEmpty())
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
        const bool command =
            key->matches(QKeySequence::Save) || key->matches(QKeySequence::Find) ||
            (key->key() == Qt::Key_Space && key->modifiers() == Qt::ControlModifier) ||
            key->key() == Qt::Key_F12 || key->key() == Qt::Key_F9;
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
    const bool handled = PlainTextEdit::event(event);
    if (services_ && (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange ||
                      event->type() == QEvent::PaletteChange)) {
        setTabStopDistance(fontMetrics().horizontalAdvance(u' ') * 4);
        UpdateGutter();
        HighlightLine();
    }
    return handled;
}

void CodeEditor::keyPressEvent(KeyEvent* event) {
    if (services_->completer->popup()->isVisible() &&
        (event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return ||
         event->key() == Qt::Key_Tab || event->key() == Qt::Key_Escape)) {
        event->ignore();
        return;
    }
    if (event->key() == Qt::Key_F12) {
        Q_EMIT DefinitionRequested();
        return;
    }
    if (event->key() == Qt::Key_F9) {
        ToggleBreakpoint(CurrentLine());
        return;
    }
    if (event->key() == Qt::Key_Space && event->modifiers() == Qt::ControlModifier) {
        Q_EMIT CompletionRequested();
        return;
    }
    if (event->matches(QKeySequence::Save)) {
        Q_EMIT SaveRequested();
        return;
    }
    if (event->matches(QKeySequence::Find)) {
        Q_EMIT FindRequested();
        return;
    }
    if (isReadOnly()) {
        PlainTextEdit::keyPressEvent(event);
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
    if (!event->text().isEmpty() &&
        (event->text() == QStringLiteral(".") || CurrentWord().size() >= 2))
        services_->completionTimer.start();
}

String CodeEditor::CurrentWord() const {
    const auto prefix = textCursor().block().text().left(textCursor().positionInBlock());
    static const QRegularExpression word(QStringLiteral(R"([@\p{L}_][\p{L}\p{N}_]*$)"));
    return word.match(prefix).captured();
}

bool CodeEditor::ApplyEdits(const std::vector<CodeTextEdit>& edits) {
    if (isReadOnly())
        return false;

    struct Edit {
        int from, to;
        String text;
    };

    std::vector<Edit> valid;
    for (const auto& edit : edits) {
        const auto start = document()->findBlockByNumber(edit.line - 1);
        const auto end = document()->findBlockByNumber(edit.endLine - 1);
        if (!start.isValid() || !end.isValid() || edit.column < 1 || edit.endColumn < 1 ||
            edit.column > start.length() || edit.endColumn > end.length())
            return false;
        const int from = start.position() + edit.column - 1,
                  to = end.position() + edit.endColumn - 1;
        if (from > to)
            return false;
        valid.push_back({from, to, edit.text});
    }
    std::sort(valid.begin(), valid.end(),
              [](const auto& a, const auto& b) { return a.from > b.from; });
    for (size_t i = 1; i < valid.size(); ++i)
        if (valid[i].to > valid[i - 1].from)
            return false;
    auto caret = textCursor();
    auto cursor = caret;
    cursor.beginEditBlock();
    for (const auto& edit : valid) {
        cursor.setPosition(edit.from);
        cursor.setPosition(edit.to, QTextCursor::KeepAnchor);
        cursor.insertText(edit.text);
    }
    cursor.endEditBlock();
    setTextCursor(caret);
    services_->completer->popup()->hide();
    return true;
}

void CodeEditor::ShowCompletions(const std::vector<CodeCompletion>& items) {
    if (isReadOnly())
        return;
    services_->completions = items;
    services_->completionRevision = document()->revision();
    auto* model = new QStandardItemModel(services_->completer);
    for (size_t i = 0; i < items.size(); ++i) {
        auto* row = new QStandardItem(items[i].label);
        row->setData(int(i), Qt::UserRole);
        row->setToolTip(items[i].detail);
        model->appendRow(row);
    }
    auto* old = services_->completer->model();
    services_->completer->setModel(model);
    if (old)
        old->deleteLater();
    services_->completer->setCompletionPrefix(CurrentWord());
    if (items.empty()) {
        services_->completer->popup()->hide();
        return;
    }
    auto rect = cursorRect();
    rect.setWidth(std::min(500, std::max(240, width() / 2)));
    services_->completer->complete(rect);
}

void CodeEditor::ClickGutter(int y, bool condition) {
    const int line = cursorForPosition(QPoint(0, y)).blockNumber() + 1;
    if (condition)
        Q_EMIT BreakpointConditionRequested(line);
    else
        ToggleBreakpoint(line);
}

void CodeEditor::ToggleBreakpoint(int line) {
    const auto marker =
        std::find_if(services_->markers.begin(), services_->markers.end(),
                     [line](const auto& value) { return value.cursor.blockNumber() + 1 == line; });
    if (marker != services_->markers.end())
        services_->markers.erase(marker);
    else {
        const auto block = document()->findBlockByNumber(line - 1);
        if (!block.isValid())
            return;
        services_->markers.push_back({QTextCursor(block), {}, false});
    }
    gutter_->update();
    Q_EMIT BreakpointsChanged();
}

std::vector<CodeBreakpoint> CodeEditor::Breakpoints() const {
    std::vector<CodeBreakpoint> result;
    for (const auto& marker : services_->markers)
        result.push_back({marker.cursor.blockNumber() + 1, marker.condition, marker.verified});
    return result;
}

void CodeEditor::SetBreakpoints(const std::vector<CodeBreakpoint>& breakpoints) {
    services_->markers.clear();
    for (const auto& point : breakpoints) {
        const auto block = document()->findBlockByNumber(point.line - 1);
        if (block.isValid())
            services_->markers.push_back({QTextCursor(block), point.condition, point.verified});
    }
    gutter_->update();
}

void CodeEditor::SetExecutionLine(int line) {
    services_->executionLine = line;
    if (line > 0)
        GoTo(line);
    gutter_->update();
}
} // namespace LuxUI
