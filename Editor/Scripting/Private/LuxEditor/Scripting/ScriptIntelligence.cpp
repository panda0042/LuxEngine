#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scripting/CSharpLanguageClient.hpp>
#include <LuxEditor/Scripting/ScriptDebugger.hpp>
#include <LuxEditor/Scripting/ScriptWorkspace.hpp>
#include <algorithm>

namespace LuxEditor {
namespace {
LuxUI::String PathText(const std::filesystem::path& path) {
    return LuxUI::String::fromUtf8(PathUtf8(path));
}

LuxUI::CodeTextEdit Edit(const LuxUI::JsonObject& value) {
    const auto range = value.value("range").toObject();
    const auto start = range.value("start").toObject(), end = range.value("end").toObject();
    return {start.value("line").toInt() + 1, start.value("character").toInt() + 1,
            end.value("line").toInt() + 1, end.value("character").toInt() + 1,
            value.value("newText").toString()};
}
} // namespace

void ScriptWorkspace::RestartLanguage() {
    if (root_.empty() || sources_.empty()) {
        language_->Stop();
        return;
    }
    language_->Start(PathText(root_));
    for (const auto& source : sources_)
        language_->Update(LuxUI::Url::fromLocalFile(PathText(source.path)).toString(),
                          source.editor->toPlainText(), source.version);
}

void ScriptWorkspace::SourceChanged(LuxUI::CodeEditor* editor) {
    for (auto& source : sources_)
        if (source.editor == editor) {
            ++source.version;
            language_->Update(LuxUI::Url::fromLocalFile(PathText(source.path)).toString(),
                              editor->toPlainText(), source.version);
            break;
        }
}

void ScriptWorkspace::CompleteCode() {
    auto* editor = CurrentEditor();
    if (!editor || root_.empty())
        return;
    RestartLanguage();
    const auto& source = sources_[tabs_->currentIndex()];
    const auto path = source.path;
    const int version = source.version, line = editor->CurrentLine(),
              column = editor->CurrentColumn();
    const int prefix = editor->CurrentWord().size();
    language_->Complete(
        LuxUI::Url::fromLocalFile(PathText(path)).toString(), line, column,
        [this, path, version, line, column, prefix](const auto& result) {
            const auto found =
                std::find_if(sources_.begin(), sources_.end(),
                             [&](const auto& source) { return source.path == path; });
            if (found == sources_.end() || found->version != version ||
                CurrentEditor() != found->editor || found->editor->CurrentLine() != line ||
                found->editor->CurrentColumn() != column)
                return;
            const auto items =
                result.isArray() ? result.toArray() : result.toObject().value("items").toArray();
            std::vector<LuxUI::CodeCompletion> completions;
            for (const auto& value : items) {
                const auto item = value.toObject();
                if (item.value("insertTextFormat").toInt(1) == 2)
                    continue;
                LuxUI::CodeCompletion completion;
                completion.label = item.value("label").toString();
                completion.detail = item.value("detail").toString();
                auto edit = item.value("textEdit").toObject();
                if (!edit.isEmpty()) {
                    if (!edit.contains("range"))
                        edit.insert("range", edit.value("replace"));
                    completion.edits.push_back(Edit(edit));
                } else
                    completion.edits.push_back(
                        {line, column - prefix, line, column,
                         item.value("insertText").toString(completion.label)});
                for (const auto& additional : item.value("additionalTextEdits").toArray())
                    completion.edits.push_back(Edit(additional.toObject()));
                completions.push_back(std::move(completion));
                if (completions.size() >= 500)
                    break;
            }
            found->editor->ShowCompletions(completions);
        });
}

void ScriptWorkspace::GoToDefinition() {
    auto* editor = CurrentEditor();
    if (!editor || root_.empty())
        return;
    RestartLanguage();
    const auto path = sources_[tabs_->currentIndex()].path;
    const int version = sources_[tabs_->currentIndex()].version;
    language_->Definition(
        LuxUI::Url::fromLocalFile(PathText(path)).toString(), editor->CurrentLine(),
        editor->CurrentColumn(), [this, path, version](const auto& result) {
            const auto found =
                std::find_if(sources_.begin(), sources_.end(),
                             [&](const auto& source) { return source.path == path; });
            if (found == sources_.end() || found->version != version ||
                CurrentEditor() != found->editor)
                return;
            const auto array = result.toArray();
            const auto location =
                result.isArray() ? (array.isEmpty() ? LuxUI::JsonObject{} : array[0].toObject())
                                 : result.toObject();
            const auto uri =
                LuxUI::Url(location.value("uri").toString(location.value("targetUri").toString()));
            if (!uri.isLocalFile()) {
                status_->setText(LUX_UI_TEXT("이 심볼의 로컬 소스 정의를 찾지 못했습니다."));
                return;
            }
            const auto range =
                location.value("targetSelectionRange").toObject(location.value("range").toObject());
            const auto start = range.value("start").toObject();
            std::string error;
            if (!OpenFile(PathFromUtf8(uri.toLocalFile().toUtf8().toStdString()), error,
                          start.value("line").toInt() + 1, start.value("character").toInt() + 1))
                ShowFailure(error);
        });
}

void ScriptWorkspace::ConfigureScriptTools() {
    if (debugger_->Active()) {
        status_->setText(LUX_UI_TEXT("디버깅을 끝낸 뒤 도구 경로를 바꿔 주세요."));
        return;
    }
    LuxUI::Dialog dialog(this);
    dialog.setWindowTitle(LUX_UI_TEXT("C# 개발 도구"));
    auto* layout = new LuxUI::VBoxLayout(&dialog);
    auto* form = new LuxUI::FormLayout;
    layout->addLayout(form);
    auto* server = new LuxUI::LineEdit(
        FindScriptTool("LUX_CSHARP_LS", "scripts/languageServer", "csharp-ls"), &dialog);
    auto* adapter = new LuxUI::LineEdit(
        FindScriptTool("LUX_NETCOREDBG", "scripts/debugAdapter", "netcoredbg"), &dialog);
    for (const auto& field : {std::pair{server, LUX_UI_TEXT("csharp-ls")},
                              std::pair{adapter, LUX_UI_TEXT("netcoredbg")}}) {
        auto* row = new LuxUI::Widget(&dialog);
        auto* horizontal = new LuxUI::HBoxLayout(row);
        horizontal->setContentsMargins(0, 0, 0, 0);
        horizontal->addWidget(field.first, 1);
        auto* choose = new LuxUI::PushButton(LUX_UI_TEXT("찾기…"), row);
        horizontal->addWidget(choose);
        connect(choose, &LuxUI::PushButton::clicked, &dialog, [&, input = field.first] {
            const auto path = LuxUI::FileDialog::getOpenFileName(
                &dialog, LUX_UI_TEXT("도구 실행 파일"), input->text());
            if (!path.isEmpty())
                input->setText(path);
        });
        form->addRow(field.second, row);
    }
    auto* buttons = new LuxUI::DialogButtonBox(
        LuxUI::DialogButtonBox::Ok | LuxUI::DialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    connect(buttons, &LuxUI::DialogButtonBox::accepted, &dialog, &LuxUI::Dialog::accept);
    connect(buttons, &LuxUI::DialogButtonBox::rejected, &dialog, &LuxUI::Dialog::reject);
    dialog.resize(700, dialog.sizeHint().height());
    if (dialog.exec() != LuxUI::Dialog::Accepted)
        return;
    LuxUI::Settings().setValue(LUX_UI_TEXT("scripts/languageServer"), server->text().trimmed());
    LuxUI::Settings().setValue(LUX_UI_TEXT("scripts/debugAdapter"), adapter->text().trimmed());
    language_->Stop();
    RestartLanguage();
}
} // namespace LuxEditor
