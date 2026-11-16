#include <LuxEditor/Build/BuildOutput.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scripting/CSharpLanguageClient.hpp>
#include <LuxEditor/Scripting/ScriptDebugger.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <LuxEditor/Ui/IconFactory.hpp>
#include <algorithm>
#include <regex>

namespace LuxEditor {
namespace {
constexpr qint64 MaxSourceBytes = 4 * 1024 * 1024;

LuxUI::String TextPath(const std::filesystem::path& path) {
    return LuxUI::String::fromUtf8(PathUtf8(path));
}

bool ReadSource(const std::filesystem::path& path, LuxUI::ByteArray& bytes, LuxUI::String& text,
                std::string& error) {
    LuxUI::File file(TextPath(path));
    if (!file.open(LuxUI::File::ReadOnly)) {
        error = file.errorString().toUtf8().toStdString();
        return false;
    }
    if (file.size() > MaxSourceBytes) {
        error = "스크립트 편집기는 4 MiB 이하의 UTF-8 파일을 지원합니다.";
        return false;
    }
    bytes = file.readAll();
    LuxUI::StringDecoder decoder(LuxUI::StringConverter::Utf8,
                                 LuxUI::StringConverter::Flag::Stateless);
    text = decoder.decode(bytes);
    if (file.error() != LuxUI::File::NoError || decoder.hasError() || text.contains(u'\0')) {
        error = "UTF-8 텍스트 파일을 읽지 못했습니다.";
        return false;
    }
    if (text.startsWith(LuxUI::Char(0xfeff)))
        text.remove(0, 1);
    text.replace(LUX_UI_TEXT("\r\n"), LUX_UI_TEXT("\n"));
    return true;
}
} // namespace

ScriptWorkspace::ScriptWorkspace(LuxUI::Widget* parent) : LuxUI::Widget(parent) {
    setObjectName(LUX_UI_TEXT("scriptWorkspace"));
    auto* layout = new LuxUI::VBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* toolbar = new LuxUI::ToolBar(this);
    toolbar->setObjectName(LUX_UI_TEXT("scriptToolbar"));
    toolbar->setIconSize({18, 18});
    layout->addWidget(toolbar);
    const auto button = [this, toolbar](const LuxUI::String& label, EditorIcon icon, const char* id,
                                        const std::function<void()>& action) {
        auto* control = new LuxUI::ToolButton(toolbar);
        control->setText(label);
        control->setIcon(MakeEditorIcon(icon));
        control->setToolButtonStyle(LuxUI::Flags::ToolButtonTextBesideIcon);
        control->setObjectName(LuxUI::String::fromLatin1(id));
        connect(control, &LuxUI::ToolButton::clicked, this, action);
        toolbar->addWidget(control);
        return control;
    };
    create_ = button(LUX_UI_TEXT("새 C#"), EditorIcon::Sprite, "newScript", [this] {
        bool accepted = false;
        const auto name = LuxUI::InputDialog::getText(
            this, LUX_UI_TEXT("새 C# 스크립트"), LUX_UI_TEXT("클래스 이름"),
            LuxUI::LineEdit::Normal, LUX_UI_TEXT("NewBehaviour"), &accepted);
        if (accepted) {
            std::string error;
            if (!CreateScript(name.trimmed().toUtf8().toStdString(), error))
                ShowFailure(error);
        }
    });
    button(LUX_UI_TEXT("열기"), EditorIcon::Folder, "openScript", [this] {
        const auto file = LuxUI::FileDialog::getOpenFileName(
            this, LUX_UI_TEXT("스크립트 열기"), TextPath(root_), LUX_UI_TEXT("C# (*.cs)"));
        std::string error;
        if (!file.isEmpty() && !OpenFile(PathFromUtf8(file.toUtf8().toStdString()), error))
            ShowFailure(error);
    });
    button(LUX_UI_TEXT("저장"), EditorIcon::Save, "saveScript", [this] {
        std::string error;
        if (!SaveFile(tabs_->currentIndex(), error))
            ShowFailure(error);
    });
    button(LUX_UI_TEXT("모두 저장"), EditorIcon::Save, "saveAllScripts", [this] { SaveAll(); });
    toolbar->addSeparator();
    button(LUX_UI_TEXT("다시 읽기"), EditorIcon::Folder, "reloadScript", [this] {
        if (CurrentEditor() &&
            (!CurrentEditor()->IsModified() ||
             LuxUI::MessageBox::question(
                 this, LUX_UI_TEXT("파일 다시 읽기"),
                 LUX_UI_TEXT("저장하지 않은 변경을 버리고 다시 읽으시겠습니까?")) ==
                 LuxUI::MessageBox::Yes)) {
            std::string error;
            if (!ReloadFile(tabs_->currentIndex(), error, true))
                ShowFailure(error);
        }
    });
    button(LUX_UI_TEXT("외부 편집기"), EditorIcon::Open, "externalScriptEditor", [this] {
        const int index = tabs_->currentIndex();
        std::string error;
        if (index >= 0 && SaveFile(index, error)) {
            if (!LuxUI::DesktopServices::openUrl(
                    LuxUI::Url::fromLocalFile(TextPath(sources_[index].path))))
                ShowFailure("C# 파일에 연결된 외부 편집기를 열지 못했습니다.");
        } else if (!error.empty())
            ShowFailure(error);
    });
    toolbar->addSeparator();
    build_ = button(LUX_UI_TEXT("C# 빌드"), EditorIcon::Play, "buildScripts",
                    [this] { BuildScripts(); });
    stop_ =
        button(LUX_UI_TEXT("중지"), EditorIcon::Stop, "stopScriptBuild", [this] { StopBuild(); });
    stop_->setEnabled(false);
    button(LUX_UI_TEXT("새로고침"), EditorIcon::Folder, "refreshScripts",
           [this] { RefreshFiles(); });

    searchBar_ = new LuxUI::Widget(this);
    searchBar_->setObjectName(LUX_UI_TEXT("scriptSearchBar"));
    auto* search = new LuxUI::HBoxLayout(searchBar_);
    search->setContentsMargins(6, 4, 6, 4);
    find_ = new LuxUI::LineEdit(searchBar_);
    find_->setObjectName(LUX_UI_TEXT("scriptFind"));
    find_->setPlaceholderText(LUX_UI_TEXT("찾기 (Ctrl+F)"));
    replace_ = new LuxUI::LineEdit(searchBar_);
    replace_->setPlaceholderText(LUX_UI_TEXT("바꿀 내용"));
    matchCase_ = new LuxUI::CheckBox(LUX_UI_TEXT("대소문자"), searchBar_);
    search->addWidget(find_, 1);
    search->addWidget(replace_, 1);
    search->addWidget(matchCase_);
    for (int action = 0; action < 4; ++action) {
        const LuxUI::String titles[] = {LUX_UI_TEXT("이전"), LUX_UI_TEXT("다음"),
                                        LUX_UI_TEXT("모두 바꾸기"), LUX_UI_TEXT("닫기")};
        auto* control = new LuxUI::PushButton(titles[action], searchBar_);
        search->addWidget(control);
        connect(control, &LuxUI::PushButton::clicked, this, [this, action] {
            if (action == 3)
                searchBar_->hide();
            else if (auto* editor = CurrentEditor()) {
                if (action == 2)
                    status_->setText(LUX_UI_TEXT("%1개 변경")
                                         .arg(editor->ReplaceAll(find_->text(), replace_->text(),
                                                                 matchCase_->isChecked())));
                else if (!editor->FindText(find_->text(), action == 0, matchCase_->isChecked()))
                    status_->setText(LUX_UI_TEXT("검색 결과 없음"));
            }
        });
    }
    connect(find_, &LuxUI::LineEdit::returnPressed, this, [this] {
        if (auto* editor = CurrentEditor())
            editor->FindText(find_->text(), false, matchCase_->isChecked());
    });
    layout->addWidget(searchBar_);
    searchBar_->hide();

    auto* vertical = new LuxUI::Splitter(LuxUI::Flags::Vertical, this);
    auto* main = new LuxUI::Splitter(vertical);
    files_ = new LuxUI::TreeWidget(main);
    files_->setObjectName(LUX_UI_TEXT("scriptFiles"));
    files_->setHeaderLabel(LUX_UI_TEXT("스크립트 파일"));
    files_->setMinimumWidth(150);
    tabs_ = new LuxUI::TabWidget(main);
    tabs_->setObjectName(LUX_UI_TEXT("scriptDocuments"));
    tabs_->setTabsClosable(true);
    tabs_->setDocumentMode(true);
    tabs_->setMovable(false);
    outline_ = new LuxUI::TreeWidget(main);
    outline_->setObjectName(LUX_UI_TEXT("scriptOutline"));
    outline_->setHeaderLabel(LUX_UI_TEXT("클래스 · 함수"));
    outline_->setRootIsDecorated(false);
    main->setStretchFactor(1, 1);
    main->setSizes({200, 900, 200});
    auto* bottom = new LuxUI::TabWidget(vertical);
    problems_ = new LuxUI::ListWidget(bottom);
    problems_->setObjectName(LUX_UI_TEXT("scriptProblems"));
    output_ = new LuxUI::PlainTextEdit(bottom);
    output_->setObjectName(LUX_UI_TEXT("scriptBuildOutput"));
    output_->setReadOnly(true);
    output_->setMaximumBlockCount(2000);
    bottom->addTab(problems_, LUX_UI_TEXT("문제"));
    bottom->addTab(output_, LUX_UI_TEXT("빌드 출력"));
    vertical->setStretchFactor(0, 1);
    vertical->setSizes({650, 150});
    layout->addWidget(vertical, 1);
    auto* footer = new LuxUI::Widget(this);
    auto* footerLayout = new LuxUI::HBoxLayout(footer);
    footerLayout->setContentsMargins(8, 3, 8, 3);
    filePath_ = new LuxUI::Label(footer);
    filePath_->setMinimumWidth(0);
    filePath_->setSizePolicy(LuxUI::SizePolicy::Ignored, LuxUI::SizePolicy::Preferred);
    filePath_->setTextInteractionFlags(LuxUI::Flags::TextSelectableByMouse);
    status_ = new LuxUI::Label(footer);
    footerLayout->addWidget(filePath_, 1);
    footerLayout->addWidget(status_);
    layout->addWidget(footer);
    outlineTimer_.setSingleShot(true);
    outlineTimer_.setInterval(200);
    connect(&outlineTimer_, &LuxUI::Timer::timeout, this, [this] { RefreshOutline(); });
    connect(tabs_, &LuxUI::TabWidget::currentChanged, this, [this] {
        RefreshOutline();
        UpdateStatus();
    });
    connect(tabs_, &LuxUI::TabWidget::tabCloseRequested, this,
            [this](int index) { CloseFile(index); });
    connect(files_, &LuxUI::TreeWidget::itemActivated, this, [this](LuxUI::TreeWidgetItem* item) {
        const auto path = item->data(0, LuxUI::Flags::UserRole).toString();
        std::string error;
        if (!path.isEmpty() && !OpenFile(PathFromUtf8(path.toUtf8().toStdString()), error))
            ShowFailure(error);
    });
    connect(outline_, &LuxUI::TreeWidget::itemActivated, this, [this](LuxUI::TreeWidgetItem* item) {
        if (auto* editor = CurrentEditor())
            editor->GoTo(item->data(0, LuxUI::Flags::UserRole).toInt());
    });
    connect(problems_, &LuxUI::ListWidget::itemActivated, this,
            [this](LuxUI::ListWidgetItem* item) {
                const auto path = item->data(LuxUI::Flags::UserRole).toString();
                std::string error;
                if (!path.isEmpty() && !OpenFile(PathFromUtf8(path.toUtf8().toStdString()), error,
                                                 item->data(LuxUI::Flags::UserRole + 1).toInt(),
                                                 item->data(LuxUI::Flags::UserRole + 2).toInt()))
                    ShowFailure(error);
            });
    BuildScriptServices(toolbar, bottom);
    SetExecutionActive(false);
}

ScriptWorkspace::~ScriptWorkspace() {
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

LuxUI::CodeEditor* ScriptWorkspace::CurrentEditor() const {
    return dynamic_cast<LuxUI::CodeEditor*>(tabs_->currentWidget());
}

bool ScriptWorkspace::IsBuilding() const {
    return process_ != nullptr;
}

bool ScriptWorkspace::OpenFile(const std::filesystem::path& path, std::string& error, int line,
                               int column) {
    error.clear();
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(path, ec);
    if (ec || !std::filesystem::is_regular_file(canonical) ||
        LuxUI::String::fromUtf8(PathUtf8(canonical.extension())).toLower() != LUX_UI_TEXT(".cs")) {
        error = "C# 소스 파일을 찾지 못했습니다.";
        return false;
    }
    for (size_t i = 0; i < sources_.size(); ++i)
        if (sources_[i].path == canonical) {
            tabs_->setCurrentIndex(static_cast<int>(i));
            sources_[i].editor->GoTo(line, column);
            return true;
        }
    Source source;
    source.path = canonical;
    LuxUI::String text;
    if (!ReadSource(canonical, source.disk, text, error))
        return false;
    source.crlf = source.disk.contains("\r\n");
    source.bom = source.disk.startsWith("\xef\xbb\xbf");
    source.editor = new LuxUI::CodeEditor(tabs_);
    source.editor->SetCleanText(text);
    auto* editor = source.editor;
    sources_.push_back(std::move(source));
    const int index = tabs_->addTab(editor, TextPath(canonical.filename()));
    tabs_->setTabToolTip(index, TextPath(canonical));
    connect(editor, &LuxUI::CodeEditor::modificationChanged, this,
            [this, editor] { UpdateCaption(editor); });
    connect(editor, &LuxUI::CodeEditor::textChanged, this, [this] { outlineTimer_.start(); });
    connect(editor, &LuxUI::CodeEditor::textChanged, this,
            [this, editor] { SourceChanged(editor); });
    connect(editor, &LuxUI::CodeEditor::CompletionRequested, this, [this] { CompleteCode(); });
    connect(editor, &LuxUI::CodeEditor::DefinitionRequested, this, [this] { GoToDefinition(); });
    connect(editor, &LuxUI::CodeEditor::BreakpointsChanged, this,
            [this, editor] { SaveBreakpoints(editor); });
    connect(editor, &LuxUI::CodeEditor::BreakpointConditionRequested, this,
            [this, editor](int line) {
                auto points = editor->Breakpoints();
                const auto found =
                    std::find_if(points.begin(), points.end(),
                                 [line](const auto& point) { return point.line == line; });
                bool accepted = false;
                const auto condition = LuxUI::InputDialog::getText(
                    this, LUX_UI_TEXT("중단점 조건"), LUX_UI_TEXT("C# 식 (비우면 항상 중단)"),
                    LuxUI::LineEdit::Normal,
                    found == points.end() ? LuxUI::String() : found->condition, &accepted);
                if (!accepted)
                    return;
                if (found == points.end())
                    points.push_back({line, condition, false});
                else
                    found->condition = condition;
                editor->SetBreakpoints(points);
                SaveBreakpoints(editor);
            });
    const auto saved = LuxUI::Settings().value(LUX_UI_TEXT("scripts/breakpoints")).toByteArray();
    const auto points =
        LuxUI::JsonDocument::fromJson(saved).object().value(TextPath(canonical)).toArray();
    std::vector<LuxUI::CodeBreakpoint> breakpoints;
    for (const auto& point : points)
        breakpoints.push_back({point.toObject().value("line").toInt(),
                               point.toObject().value("condition").toString(), false});
    editor->SetBreakpoints(breakpoints);
    debugger_->SetBreakpoints(TextPath(canonical), breakpoints);
    editor->setReadOnly(debugger_->Active());
    if (!language_->Ready())
        RestartLanguage();
    language_->Update(LuxUI::Url::fromLocalFile(TextPath(canonical)).toString(),
                      editor->toPlainText(), sources_.back().version);
    connect(editor, &LuxUI::CodeEditor::cursorPositionChanged, this, [this] { UpdateStatus(); });
    connect(editor, &LuxUI::CodeEditor::FindRequested, this, [this] {
        searchBar_->show();
        find_->setFocus();
        find_->selectAll();
    });
    connect(editor, &LuxUI::CodeEditor::SaveRequested, this, [this, editor] {
        std::string error;
        if (!SaveFile(tabs_->indexOf(editor), error))
            ShowFailure(error);
    });
    tabs_->setCurrentIndex(index);
    editor->GoTo(line, column);
    RefreshOutline();
    return true;
}

bool ScriptWorkspace::SaveFile(int index, std::string& error) {
    error.clear();
    if (index < 0 || static_cast<size_t>(index) >= sources_.size())
        return true;
    auto& source = sources_[index];
    if (!source.editor->IsModified())
        return true;
    LuxUI::ByteArray bytes;
    LuxUI::String text;
    if (!ReadSource(source.path, bytes, text, error))
        return false;
    if (bytes != source.disk) {
        error = "파일이 외부에서 변경됐습니다. 변경 내용을 확인한 뒤 다시 읽어 주세요: " +
                PathUtf8(source.path);
        return false;
    }
    text = source.editor->toPlainText();
    if (source.crlf)
        text.replace(LUX_UI_TEXT("\n"), LUX_UI_TEXT("\r\n"));
    bytes = (source.bom ? LuxUI::ByteArray("\xef\xbb\xbf") : LuxUI::ByteArray()) + text.toUtf8();
    if (bytes.size() > MaxSourceBytes) {
        error = "저장할 스크립트가 4 MiB를 초과했습니다.";
        return false;
    }
    LuxUI::SaveFile file(TextPath(source.path));
    if (!file.open(LuxUI::File::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = "스크립트를 저장하지 못했습니다: " + file.errorString().toUtf8().toStdString();
        return false;
    }
    source.disk = bytes;
    source.editor->MarkSaved();
    return true;
}

bool ScriptWorkspace::ReloadFile(int index, std::string& error, bool discard) {
    error.clear();
    if (index < 0 || static_cast<size_t>(index) >= sources_.size())
        return false;
    auto& source = sources_[index];
    if (source.editor->IsModified() && !discard) {
        error = "저장하지 않은 변경이 있습니다.";
        return false;
    }
    LuxUI::ByteArray bytes;
    LuxUI::String text;
    if (!ReadSource(source.path, bytes, text, error))
        return false;
    source.disk = bytes;
    source.crlf = bytes.contains("\r\n");
    source.bom = bytes.startsWith("\xef\xbb\xbf");
    source.editor->SetCleanText(text);
    return true;
}

bool ScriptWorkspace::SaveAll() {
    for (size_t i = 0; i < sources_.size(); ++i) {
        std::string error;
        if (!SaveFile(static_cast<int>(i), error)) {
            tabs_->setCurrentIndex(static_cast<int>(i));
            ShowFailure(error);
            return false;
        }
    }
    return true;
}

bool ScriptWorkspace::ConfirmCloseAll() {
    const auto dirty = std::count_if(sources_.begin(), sources_.end(), [](const Source& source) {
        return source.editor->IsModified();
    });
    if (!dirty)
        return true;
    const auto answer = LuxUI::MessageBox::question(
        this, LUX_UI_TEXT("스크립트 저장"),
        LUX_UI_TEXT("변경한 스크립트 %1개를 저장하시겠습니까?").arg(dirty),
        LuxUI::MessageBox::Save | LuxUI::MessageBox::Discard | LuxUI::MessageBox::Cancel,
        LuxUI::MessageBox::Save);
    return answer == LuxUI::MessageBox::Discard || (answer == LuxUI::MessageBox::Save && SaveAll());
}

void ScriptWorkspace::CloseFile(int index) {
    if (index < 0 || static_cast<size_t>(index) >= sources_.size())
        return;
    auto* editor = sources_[index].editor;
    if (editor->IsModified()) {
        const auto answer = LuxUI::MessageBox::question(
            this, LUX_UI_TEXT("스크립트 저장"),
            LUX_UI_TEXT("%1의 변경을 저장하시겠습니까?")
                .arg(TextPath(sources_[index].path.filename())),
            LuxUI::MessageBox::Save | LuxUI::MessageBox::Discard | LuxUI::MessageBox::Cancel);
        if (answer == LuxUI::MessageBox::Cancel)
            return;
        std::string error;
        if (answer == LuxUI::MessageBox::Save && !SaveFile(index, error)) {
            ShowFailure(error);
            return;
        }
    }
    language_->Close(LuxUI::Url::fromLocalFile(TextPath(sources_[index].path)).toString());
    sources_.erase(sources_.begin() + index);
    tabs_->removeTab(index);
    delete editor;
    RefreshOutline();
    UpdateStatus();
}

void ScriptWorkspace::UpdateCaption(LuxUI::CodeEditor* editor) {
    const int index = tabs_->indexOf(editor);
    if (index >= 0 && static_cast<size_t>(index) < sources_.size())
        tabs_->setTabText(index, TextPath(sources_[index].path.filename()) +
                                     (editor->IsModified() ? LUX_UI_TEXT(" *") : LuxUI::String()));
}

void ScriptWorkspace::UpdateStatus() {
    if (auto* editor = CurrentEditor()) {
        const auto& source = sources_[tabs_->currentIndex()];
        filePath_->setText(TextPath(source.path));
        filePath_->setToolTip(TextPath(source.path));
        status_->setText(LUX_UI_TEXT("행 %1, 열 %2  |  UTF-8%3  |  %4  |  C#")
                             .arg(editor->CurrentLine())
                             .arg(editor->CurrentColumn())
                             .arg(source.bom ? LUX_UI_TEXT(" BOM") : LuxUI::String())
                             .arg(source.crlf ? LUX_UI_TEXT("CRLF") : LUX_UI_TEXT("LF")));
    } else {
        filePath_->clear();
        status_->clear();
    }
}

void ScriptWorkspace::RefreshOutline() {
    outline_->clear();
    if (auto* editor = CurrentEditor())
        for (const auto& symbol : editor->Outline()) {
            auto* item = new LuxUI::TreeWidgetItem(outline_, {symbol.name});
            item->setData(0, LuxUI::Flags::UserRole, symbol.line);
            item->setToolTip(0, LUX_UI_TEXT("행 %1").arg(symbol.line));
        }
}

void ScriptWorkspace::ShowFailure(const std::string& error) {
    LuxUI::MessageBox::warning(this, LUX_UI_TEXT("스크립트 편집기"),
                               LuxUI::String::fromUtf8(error));
}

void ScriptWorkspace::SetProjectRoot(const std::filesystem::path& root) {
    if (root_ == root)
        return;
    if (debugger_->Active())
        StopDebugger();
    StopBuild();
    root_ = root;
    RestartLanguage();
    RefreshFiles();
    SetExecutionActive(executionActive_);
}

void ScriptWorkspace::SetExecutionActive(bool active) {
    executionActive_ = active;
    build_->setEnabled(!active && !process_ && !root_.empty() && !debugger_->Active());
    create_->setEnabled(!root_.empty() && !debugger_->Active());
    RefreshDebugControls();
}

void ScriptWorkspace::RefreshFiles() {
    files_->clear();
    if (root_.empty())
        return;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        root_, std::filesystem::directory_options::skip_permission_denied, error),
        end;
    std::vector<std::filesystem::path> paths;
    for (; !error && it != end && paths.size() < 2000; it.increment(error)) {
        if (it->is_symlink(error)) {
            if (it->is_directory(error))
                it.disable_recursion_pending();
            continue;
        }
        if (it->is_directory(error)) {
            const auto name = PathUtf8(it->path().filename());
            if (name.starts_with('.') || name == "bin" || name == "obj" || name == "build" ||
                name == "out" || name.starts_with("cmake-build"))
                it.disable_recursion_pending();
        } else if (it->is_regular_file(error) && it->path().extension() == ".cs")
            paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto& path : paths) {
        auto* item = new LuxUI::TreeWidgetItem(files_, {TextPath(path.lexically_relative(root_))});
        item->setData(0, LuxUI::Flags::UserRole, TextPath(path));
        item->setToolTip(0, TextPath(path));
    }
}

bool ScriptWorkspace::CreateScript(const std::string& name, std::string& error) {
    error.clear();
    static const std::regex identifier("[A-Za-z_][A-Za-z_0-9]{0,63}");
    static const std::regex keyword(
        "abstract|as|base|bool|break|byte|case|catch|char|checked|class|const|continue|decimal|"
        "default|delegate|do|double|else|enum|event|explicit|extern|false|finally|fixed|float|"
        "for|foreach|goto|if|implicit|in|int|interface|internal|is|lock|long|namespace|new|"
        "null|object|operator|out|override|params|private|protected|public|readonly|ref|return|"
        "sbyte|sealed|short|sizeof|stackalloc|static|string|struct|switch|this|throw|true|try|"
        "typeof|uint|ulong|unchecked|unsafe|ushort|using|virtual|void|volatile|while");
    if (root_.empty() || !std::regex_match(name, identifier) || std::regex_match(name, keyword)) {
        error = "프로젝트를 열고 영문·숫자·밑줄로 된 클래스 이름을 입력해 주세요.";
        return false;
    }
    const auto path = root_ / "Scripts" / (name + ".cs");
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec || std::filesystem::exists(path)) {
        error = "같은 이름의 스크립트가 있거나 Scripts 폴더를 만들지 못했습니다.";
        return false;
    }
    const auto source =
        LUX_UI_TEXT("using LuxEngine;\n\nnamespace Game {\n\n"
                    "public sealed class %1 : EntityScript {\n"
                    "    protected override void OnCreate() {\n    }\n\n"
                    "    protected override void OnUpdate(double deltaTime) {\n    }\n"
                    "}\n\n}\n")
            .arg(LuxUI::String::fromUtf8(name))
            .toUtf8();
    LuxUI::File file(TextPath(path));
    if (!file.open(LuxUI::File::WriteOnly | LuxUI::File::NewOnly)) {
        error = file.errorString().toUtf8().toStdString();
        return false;
    }
    if (file.write(source) != source.size() || !file.flush()) {
        error = file.errorString().toUtf8().toStdString();
        file.remove();
        return false;
    }
    file.close();
    RefreshFiles();
    return OpenFile(path, error);
}

void ScriptWorkspace::AddDiagnostic(const BuildDiagnostic& diagnostic,
                                    const std::filesystem::path& relativeRoot) {
    if (problems_->count() >= 500 || diagnostic.severity == DiagnosticSeverity::Note)
        return;
    auto path = PathFromUtf8(diagnostic.file);
    if (path.is_relative())
        path = (relativeRoot.empty() ? root_ : relativeRoot) / path;
    auto* item = new LuxUI::ListWidgetItem(LUX_UI_TEXT("%1(%2,%3): %4 %5")
                                               .arg(TextPath(path.filename()))
                                               .arg(diagnostic.line)
                                               .arg(diagnostic.column)
                                               .arg(LuxUI::String::fromUtf8(diagnostic.code),
                                                    LuxUI::String::fromUtf8(diagnostic.message)),
                                           problems_);
    item->setData(LuxUI::Flags::UserRole, TextPath(path));
    item->setData(LuxUI::Flags::UserRole + 1, diagnostic.line);
    item->setData(LuxUI::Flags::UserRole + 2, diagnostic.column);
    item->setToolTip(TextPath(path));
    item->setIcon(MakeEditorIcon(EditorIcon::Bug));
}

void ScriptWorkspace::BuildScripts() {
    if (executionActive_ || process_ || root_.empty() || debugger_->Active() || !SaveAll())
        return;
    const auto project = root_ / "Scripts/GameScripts.csproj";
    const auto core =
        PathFromUtf8(LuxUI::CoreApplication::applicationDirPath().toUtf8().toStdString()) /
        "managed/LuxEngine.ScriptCore.dll";
    if (!std::filesystem::is_regular_file(project) || !std::filesystem::is_regular_file(core)) {
        ShowFailure("Scripts/GameScripts.csproj 또는 에디터의 managed/LuxEngine.ScriptCore.dll을 "
                    "찾지 못했습니다.");
        return;
    }
    problems_->clear();
    output_->clear();
    buildBuffer_.clear();
    process_ = new LuxUI::Process(this);
    process_->setWorkingDirectory(TextPath(root_));
    process_->setProcessEnvironment(Utf8BuildEnvironment());
    process_->setProcessChannelMode(LuxUI::Process::MergedChannels);
    process_->setProgram(LUX_UI_TEXT("dotnet"));
    LuxUI::StringList arguments{LUX_UI_TEXT("build"),
                                TextPath(project),
                                LUX_UI_TEXT("--configuration"),
                                LUX_UI_TEXT("Debug"),
                                LUX_UI_TEXT("--output"),
                                TextPath(root_ / ".lux/managed"),
                                LUX_UI_TEXT("--artifacts-path"),
                                TextPath(root_ / ".lux/script-artifacts"),
                                LUX_UI_TEXT("-p:LuxEngineScriptCore=") + TextPath(core),
                                LUX_UI_TEXT("-p:NuGetAudit=false")};
    if (std::filesystem::is_regular_file(root_ / "Scripts/NuGet.Config"))
        arguments << LUX_UI_TEXT("-p:RestoreConfigFile=") +
                         TextPath(root_ / "Scripts/NuGet.Config");
    process_->setArguments(arguments);
    connect(process_, &LuxUI::Process::readyReadStandardOutput, this,
            [this] { ReadBuildOutput(); });
    connect(process_, qOverload<int, LuxUI::Process::ExitStatus>(&LuxUI::Process::finished), this,
            [this](int code, LuxUI::Process::ExitStatus result) {
                ReadBuildOutput(true);
                status_->setText(code == 0 && result == LuxUI::Process::NormalExit
                                     ? LUX_UI_TEXT("C# 빌드 완료")
                                     : LUX_UI_TEXT("C# 빌드 실패"));
                auto* finished = process_;
                process_ = nullptr;
                finished->deleteLater();
                stop_->setEnabled(false);
                SetExecutionActive(executionActive_);
                const bool debug = debugAfterBuild_;
                debugAfterBuild_ = false;
                if (debug && code == 0 && result == LuxUI::Process::NormalExit && debugRequested)
                    debugRequested();
            });
    connect(
        process_, &LuxUI::Process::errorOccurred, this, [this](LuxUI::Process::ProcessError error) {
            if (error == LuxUI::Process::FailedToStart) {
                output_->appendPlainText(
                    LUX_UI_TEXT("dotnet을 실행하지 못했습니다. .NET SDK 경로를 확인해 주세요."));
                auto* failed = process_;
                process_ = nullptr;
                failed->deleteLater();
                stop_->setEnabled(false);
                SetExecutionActive(executionActive_);
            }
        });
    build_->setEnabled(false);
    stop_->setEnabled(true);
    status_->setText(LUX_UI_TEXT("C# 빌드 중…"));
    process_->start();
}

void ScriptWorkspace::ReadBuildOutput(bool flush) {
    if (!process_)
        return;
    buildBuffer_ += process_->readAllStandardOutput();
    while (!buildBuffer_.isEmpty()) {
        auto end = buildBuffer_.indexOf('\n');
        if (end < 0) {
            if (!flush)
                break;
            end = buildBuffer_.size();
        }
        const auto line = DecodeBuildOutput(buildBuffer_.left(end)).trimmed();
        buildBuffer_.remove(0, end + 1);
        output_->appendPlainText(line);
        if (const auto diagnostic = ParseBuildLine(line.toUtf8().toStdString()))
            AddDiagnostic(*diagnostic);
    }
}

void ScriptWorkspace::StopBuild() {
    if (!process_)
        return;
    process_->disconnect(this);
    process_->kill();
    process_->waitForFinished(1000);
    ReadBuildOutput(true);
    process_->deleteLater();
    process_ = nullptr;
    stop_->setEnabled(false);
    status_->setText(LUX_UI_TEXT("C# 빌드 중지"));
    SetExecutionActive(executionActive_);
}
} // namespace LuxEditor
