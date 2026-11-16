#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scripting/CSharpLanguageClient.hpp>
#include <LuxEditor/Scripting/ScriptDebugger.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <LuxEditor/Ui/IconFactory.hpp>
#include <algorithm>

namespace LuxEditor {
void ScriptWorkspace::BuildScriptServices(LuxUI::ToolBar* toolbar, LuxUI::TabWidget* bottom) {
    language_ = std::make_unique<CSharpLanguageClient>(this);
    debugger_ = std::make_unique<ScriptDebugger>(this);
    language_->status = [this](const auto& text) { status_->setText(text); };
    language_->diagnostics = [this](const auto& message) {
        const auto file = LuxUI::Url(message.value("uri").toString()).toLocalFile();
        for (int i = problems_->count() - 1; i >= 0; --i)
            if (problems_->item(i)->data(LuxUI::Flags::UserRole).toString() == file &&
                problems_->item(i)->data(LuxUI::Flags::UserRole + 3).toBool())
                delete problems_->takeItem(i);
        for (const auto& item : message.value("diagnostics").toArray()) {
            const auto diagnostic = item.toObject();
            const auto start = diagnostic.value("range").toObject().value("start").toObject();
            BuildDiagnostic parsed;
            parsed.file = file.toUtf8().toStdString();
            parsed.line = start.value("line").toInt() + 1;
            parsed.column = start.value("character").toInt() + 1;
            parsed.severity = diagnostic.value("severity").toInt(1) == 1
                                  ? DiagnosticSeverity::Error
                                  : DiagnosticSeverity::Warning;
            parsed.code = diagnostic.value("code").toVariant().toString().toUtf8().toStdString();
            parsed.message = diagnostic.value("message").toString().toUtf8().toStdString();
            const int count = problems_->count();
            AddDiagnostic(parsed);
            if (problems_->count() > count)
                problems_->item(problems_->count() - 1)->setData(LuxUI::Flags::UserRole + 3, true);
        }
    };
    debugger_->status = [this](const auto& message) { status_->setText(message); };
    debugger_->event = [this](const auto& event) { OnDebugEvent(event); };
    debugger_->verified = [this](const auto& path, const auto& points) {
        for (const auto& source : sources_)
            if (LuxUI::String::fromUtf8(PathUtf8(source.path)) == path)
                source.editor->SetBreakpoints(points);
    };
    const auto saved =
        LuxUI::JsonDocument::fromJson(
            LuxUI::Settings().value(LUX_UI_TEXT("scripts/breakpoints")).toByteArray())
            .object();
    for (auto it = saved.begin(); it != saved.end(); ++it) {
        std::vector<LuxUI::CodeBreakpoint> points;
        for (const auto& point : it.value().toArray())
            points.push_back({point.toObject().value("line").toInt(),
                              point.toObject().value("condition").toString(), false});
        debugger_->SetBreakpoints(it.key(), points);
    }
    toolbar->addSeparator();
    auto* tools = new LuxUI::ToolButton(toolbar);
    tools->setText(LUX_UI_TEXT("C# 도구 설정"));
    tools->setObjectName(LUX_UI_TEXT("scriptToolSettings"));
    toolbar->addWidget(tools);
    connect(tools, &LuxUI::ToolButton::clicked, this, [this] { ConfigureScriptTools(); });
    auto* debug = new LuxUI::Widget(bottom);
    auto* layout = new LuxUI::VBoxLayout(debug);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* bar = new LuxUI::ToolBar(debug);
    layout->addWidget(bar);
    const char* names[] = {"디버그 실행", "계속",        "일시정지",   "프로시저 단위",
                           "한 단계씩",   "단계 나가기", "디버그 중지"};
    const char* commands[] = {"", "continue", "pause", "next", "stepIn", "stepOut", ""};
    const EditorIcon icons[] = {EditorIcon::Bug,  EditorIcon::Play, EditorIcon::Pause,
                                EditorIcon::Step, EditorIcon::Step, EditorIcon::Step,
                                EditorIcon::Stop};
    for (int i = 0; i < 7; ++i) {
        auto* control = new LuxUI::ToolButton(bar);
        control->setText(LuxUI::String::fromUtf8(names[i]));
        control->setIcon(MakeEditorIcon(icons[i]));
        control->setToolButtonStyle(LuxUI::Flags::ToolButtonTextBesideIcon);
        control->setObjectName(LUX_UI_TEXT("scriptDebugControl%1").arg(i));
        bar->addWidget(control);
        debugControls_.push_back(control);
        connect(control, &LuxUI::ToolButton::clicked, this, [this, i, command = commands[i]] {
            if (i == 0) {
                debugAfterBuild_ = true;
                BuildScripts();
                if (!process_)
                    debugAfterBuild_ = false;
            } else if (i == 6)
                StopDebugger();
            else
                debugger_->Control(command);
        });
    }
    auto* split = new LuxUI::Splitter(debug);
    stack_ = new LuxUI::ListWidget(split);
    stack_->setObjectName(LUX_UI_TEXT("scriptCallStack"));
    variables_ = new LuxUI::TreeWidget(split);
    variables_->setObjectName(LUX_UI_TEXT("scriptVariables"));
    variables_->setHeaderLabels({LUX_UI_TEXT("이름"), LUX_UI_TEXT("값"), LUX_UI_TEXT("형식")});
    variables_->header()->setSectionResizeMode(LuxUI::HeaderView::ResizeToContents);
    variables_->header()->setStretchLastSection(true);
    layout->addWidget(split, 1);
    watch_ = new LuxUI::LineEdit(debug);
    watch_->setObjectName(LUX_UI_TEXT("scriptWatch"));
    watch_->setPlaceholderText(LUX_UI_TEXT("중단된 프레임에서 C# 식 평가 후 Enter"));
    layout->addWidget(watch_);
    connect(stack_, &LuxUI::ListWidget::itemActivated, this, [this](auto* item) {
        ShowDebugFrame(
            LuxUI::JsonDocument::fromJson(item->data(LuxUI::Flags::UserRole).toByteArray())
                .object());
    });
    connect(watch_, &LuxUI::LineEdit::returnPressed, this, [this] {
        if (!debugger_->Paused())
            return;
        const auto expression = watch_->text();
        debugger_->Request(
            "evaluate",
            {{"expression", expression}, {"frameId", debugFrame_}, {"context", "watch"}},
            [this, expression](const auto& value) {
                auto* item = new LuxUI::TreeWidgetItem(
                    variables_,
                    {expression, value.value("result").toString(), value.value("type").toString()});
                item->setData(0, LuxUI::Flags::UserRole, value.value("variablesReference").toInt());
                if (value.value("variablesReference").toInt())
                    item->setChildIndicatorPolicy(LuxUI::TreeWidgetItem::ShowIndicator);
            });
    });
    connect(variables_, &LuxUI::TreeWidget::itemExpanded, this, [this](auto* item) {
        const int reference = item->data(0, LuxUI::Flags::UserRole).toInt();
        if (!debugger_->Paused() || !reference || item->childCount())
            return;
        // Store a path rather than a raw row pointer across asynchronous debugger responses.
        std::vector<int> path;
        for (auto* node = item; node; node = node->parent())
            path.insert(path.begin(), node->parent() ? node->parent()->indexOfChild(node)
                                                     : variables_->indexOfTopLevelItem(node));
        const int frame = debugFrame_;
        debugger_->Request(
            "variables", {{"variablesReference", reference}},
            [this, path, frame](const auto& body) {
                if (!debugger_->Paused() || frame != debugFrame_)
                    return;
                LuxUI::TreeWidgetItem* parent = nullptr;
                for (int index : path) {
                    parent = parent ? parent->child(index) : variables_->topLevelItem(index);
                    if (!parent)
                        return;
                }
                for (const auto& value : body.value("variables").toArray()) {
                    const auto variable = value.toObject();
                    auto* child =
                        new LuxUI::TreeWidgetItem(parent, {variable.value("name").toString(),
                                                           variable.value("value").toString(),
                                                           variable.value("type").toString()});
                    child->setData(0, LuxUI::Flags::UserRole,
                                   variable.value("variablesReference").toInt());
                    if (variable.value("variablesReference").toInt())
                        child->setChildIndicatorPolicy(LuxUI::TreeWidgetItem::ShowIndicator);
                }
            });
    });
    bottom->addTab(debug, LUX_UI_TEXT("스크립트 디버거"));
    RefreshDebugControls();
}

void ScriptWorkspace::SaveBreakpoints(LuxUI::CodeEditor* editor) {
    const auto found = std::find_if(sources_.begin(), sources_.end(), [editor](const auto& source) {
        return source.editor == editor;
    });
    if (found == sources_.end())
        return;
    const auto path = LuxUI::String::fromUtf8(PathUtf8(found->path));
    LuxUI::JsonArray points;
    for (const auto& point : editor->Breakpoints())
        points.append(LuxUI::JsonObject{{"line", point.line}, {"condition", point.condition}});
    auto settings = LuxUI::Settings();
    auto saved = LuxUI::JsonDocument::fromJson(
                     settings.value(LUX_UI_TEXT("scripts/breakpoints")).toByteArray())
                     .object();
    saved.insert(path, points);
    settings.setValue(LUX_UI_TEXT("scripts/breakpoints"),
                      LuxUI::JsonDocument(saved).toJson(LuxUI::JsonDocument::Compact));
    debugger_->SetBreakpoints(path, editor->Breakpoints());
}

void ScriptWorkspace::AttachDebugger(qint64 processId) {
    if (!debugger_->Attach(processId, LuxUI::String::fromUtf8(PathUtf8(root_)))) {
        if (debugStopped)
            debugStopped();
    }
    RefreshDebugControls();
}

bool ScriptWorkspace::IsDebugging() const {
    return debugger_->Active();
}

void ScriptWorkspace::StopDebugger() {
    debugger_->Stop();
    for (const auto& source : sources_) {
        source.editor->SetExecutionLine(0);
        source.editor->setReadOnly(false);
        auto points = source.editor->Breakpoints();
        for (auto& point : points)
            point.verified = false;
        source.editor->SetBreakpoints(points);
    }
    stack_->clear();
    variables_->clear();
    RefreshDebugControls();
    if (debugStopped)
        debugStopped();
}

void ScriptWorkspace::RefreshDebugControls() {
    if (debugControls_.empty())
        return;
    const bool active = debugger_->Active(), paused = debugger_->Paused();
    debugControls_[0]->setEnabled(!active && !executionActive_ && !process_ && !root_.empty());
    for (size_t i = 1; i < debugControls_.size(); ++i)
        debugControls_[i]->setEnabled(i == 6 ? active : i == 2 ? active && !paused : paused);
    watch_->setEnabled(paused);
    build_->setEnabled(!active && !executionActive_ && !process_ && !root_.empty());
    for (const auto& source : sources_)
        source.editor->setReadOnly(active);
}

void ScriptWorkspace::OnDebugEvent(const LuxUI::JsonObject& event) {
    const auto name = event.value("event").toString();
    if (name == "output")
        output_->appendPlainText(event.value("body").toObject().value("output").toString());
    else if (name == "stopped") {
        status_->setText(LUX_UI_TEXT("중단됨"));
        debugger_->Request(
            "stackTrace", {{"threadId", debugger_->ThreadId()}}, [this](const auto& body) {
                if (!debugger_->Paused())
                    return;
                stack_->clear();
                const auto frames = body.value("stackFrames").toArray();
                for (const auto& value : frames) {
                    const auto frame = value.toObject();
                    auto* item = new LuxUI::ListWidgetItem(
                        frame.value("name").toString() +
                            LUX_UI_TEXT(":%1").arg(frame.value("line").toInt()),
                        stack_);
                    item->setData(LuxUI::Flags::UserRole, LuxUI::JsonDocument(frame).toJson());
                }
                if (!frames.isEmpty()) {
                    stack_->setCurrentRow(0);
                    ShowDebugFrame(frames[0].toObject());
                }
            });
    } else if (name == "continued") {
        status_->setText(LUX_UI_TEXT("디버깅 실행 중"));
        ++debugFrame_;
        for (const auto& source : sources_)
            source.editor->SetExecutionLine(0);
        stack_->clear();
        variables_->clear();
    } else if (name == "terminated" || name == "exited") {
        StopDebugger();
        return;
    }
    RefreshDebugControls();
}

void ScriptWorkspace::ShowDebugFrame(const LuxUI::JsonObject& frame) {
    if (!debugger_->Paused())
        return;
    debugFrame_ = frame.value("id").toInt();
    const auto path = frame.value("source").toObject().value("path").toString();
    std::string error;
    if (!path.isEmpty() && OpenFile(PathFromUtf8(path.toUtf8().toStdString()), error,
                                    frame.value("line").toInt(), frame.value("column").toInt()))
        CurrentEditor()->SetExecutionLine(frame.value("line").toInt());
    variables_->clear();
    const int current = debugFrame_;
    debugger_->Request("scopes", {{"frameId", current}}, [this, current](const auto& body) {
        if (!debugger_->Paused() || current != debugFrame_)
            return;
        for (const auto& value : body.value("scopes").toArray()) {
            const auto scope = value.toObject();
            auto* item = new LuxUI::TreeWidgetItem(variables_, {scope.value("name").toString()});
            item->setData(0, LuxUI::Flags::UserRole, scope.value("variablesReference").toInt());
            item->setChildIndicatorPolicy(LuxUI::TreeWidgetItem::ShowIndicator);
            item->setExpanded(true);
        }
    });
}
} // namespace LuxEditor
