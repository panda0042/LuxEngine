#include <LuxEditor/Build/BuildOutput.hpp>
#include <LuxEditor/Scripting/CSharpLanguageClient.hpp>

namespace LuxEditor {
namespace {
LuxUI::JsonObject TextDocument(const LuxUI::String& uri) {
    return {{"uri", uri}};
}
} // namespace

CSharpLanguageClient::CSharpLanguageClient(LuxUI::Object* owner) : transport_(owner) {
    transport_.received = [this](const auto& message) { Receive(message); };
    transport_.failed = [this](const auto& error) {
        ready_ = false;
        pending_.clear();
        readyRequests_.clear();
        if (status)
            status(error);
    };
    transport_.started = [this] {
        const auto uri = LuxUI::Url::fromLocalFile(root_).toString();
        Request(
            "initialize",
            {{"processId", double(LuxUI::CoreApplication::applicationPid())},
             {"rootUri", uri},
             {"workspaceFolders", LuxUI::JsonArray{LuxUI::JsonObject{
                                      {"uri", uri}, {"name", LuxUI::FileInfo(root_).fileName()}}}},
             {"capabilities",
              LuxUI::JsonObject{
                  {"general", LuxUI::JsonObject{{"positionEncodings", LuxUI::JsonArray{"utf-16"}}}},
                  {"textDocument",
                   LuxUI::JsonObject{
                       {"completion",
                        LuxUI::JsonObject{
                            {"completionItem",
                             LuxUI::JsonObject{{"snippetSupport", false},
                                               {"insertReplaceSupport", false}}}}}}}}}},
            [this](const auto&) {
                Notify("initialized", {});
                ready_ = true;
                for (auto& [uri, document] : documents_) {
                    Notify("textDocument/didOpen",
                           {{"textDocument", LuxUI::JsonObject{{"uri", uri},
                                                               {"languageId", "csharp"},
                                                               {"version", document.version},
                                                               {"text", document.text}}}});
                    document.open = true;
                }
                if (status)
                    status(LUX_UI_TEXT("C# 언어 서버 연결됨"));
                auto queued = std::move(readyRequests_);
                readyRequests_.clear();
                for (auto& request : queued)
                    request();
            });
    };
}

CSharpLanguageClient::~CSharpLanguageClient() {
    Stop();
}

bool CSharpLanguageClient::Ready() const {
    return ready_;
}

void CSharpLanguageClient::Start(const LuxUI::String& root) {
    if (root == root_ && transport_.Running())
        return;
    Stop();
    root_ = root;
    if (root.isEmpty())
        return;
    const auto program = FindScriptTool("LUX_CSHARP_LS", "scripts/languageServer", "csharp-ls");
    if (program.isEmpty()) {
        if (status)
            status(LUX_UI_TEXT("C# 언어 서버 경로를 설정해 주세요."));
        return;
    }
    auto environment = Utf8BuildEnvironment();
    environment.insert(LUX_UI_TEXT("LuxEngineScriptCore"),
                       LuxUI::CoreApplication::applicationDirPath() +
                           LUX_UI_TEXT("/managed/LuxEngine.ScriptCore.dll"));
    transport_.Start(program, {LUX_UI_TEXT("--loglevel"), LUX_UI_TEXT("warning")}, root,
                     environment);
}

void CSharpLanguageClient::Stop() {
    transport_.Stop();
    ready_ = false;
    pending_.clear();
    readyRequests_.clear();
    documents_.clear();
}

void CSharpLanguageClient::Notify(const char* method, const LuxUI::JsonObject& parameters) {
    transport_.Send({{"jsonrpc", "2.0"},
                     {"method", LuxUI::String::fromLatin1(method)},
                     {"params", parameters}});
}

void CSharpLanguageClient::Request(const char* method, const LuxUI::JsonObject& parameters,
                                   std::function<void(const LuxUI::JsonValue&)> done) {
    const int id = nextId_++;
    pending_[id] = std::move(done);
    transport_.Send({{"jsonrpc", "2.0"},
                     {"id", id},
                     {"method", LuxUI::String::fromLatin1(method)},
                     {"params", parameters}});
}

void CSharpLanguageClient::Receive(const LuxUI::JsonObject& message) {
    if (message.contains("method")) {
        const auto method = message.value("method").toString();
        if (message.contains("id")) {
            LuxUI::JsonValue result;
            if (method == LUX_UI_TEXT("workspace/configuration")) {
                LuxUI::JsonArray values;
                for (const auto& item :
                     message.value("params").toObject().value("items").toArray()) {
                    Q_UNUSED(item);
                    values.append(LuxUI::JsonObject{});
                }
                result = values;
            } else if (method == LUX_UI_TEXT("workspace/applyEdit"))
                result = LuxUI::JsonObject{{"applied", false},
                                           {"failureReason", "Unsupported workspace edit"}};
            transport_.Send({{"jsonrpc", "2.0"}, {"id", message.value("id")}, {"result", result}});
        } else if (method == LUX_UI_TEXT("textDocument/publishDiagnostics") && diagnostics)
            diagnostics(message.value("params").toObject());
        return;
    }
    const int id = message.value("id").toInt(-1);
    const auto found = pending_.find(id);
    if (found == pending_.end())
        return;
    auto done = std::move(found->second);
    pending_.erase(found);
    if (message.contains("error")) {
        if (status)
            status(message.value("error").toObject().value("message").toString());
        if (done)
            done({});
    } else if (done)
        done(message.value("result"));
}

void CSharpLanguageClient::Update(const LuxUI::String& uri, const LuxUI::String& text,
                                  int version) {
    auto& document = documents_[uri];
    if (document.open && document.version == version)
        return;
    document.text = text;
    document.version = version;
    if (!ready_)
        return;
    if (!document.open) {
        Notify("textDocument/didOpen", {{"textDocument", LuxUI::JsonObject{{"uri", uri},
                                                                           {"languageId", "csharp"},
                                                                           {"version", version},
                                                                           {"text", text}}}});
        document.open = true;
    } else
        Notify("textDocument/didChange",
               {{"textDocument", LuxUI::JsonObject{{"uri", uri}, {"version", version}}},
                {"contentChanges", LuxUI::JsonArray{LuxUI::JsonObject{{"text", text}}}}});
}

void CSharpLanguageClient::Close(const LuxUI::String& uri) {
    if (ready_ && documents_.contains(uri))
        Notify("textDocument/didClose", {{"textDocument", TextDocument(uri)}});
    documents_.erase(uri);
}

void CSharpLanguageClient::PositionRequest(const char* method, const LuxUI::String& uri, int line,
                                           int column,
                                           std::function<void(const LuxUI::JsonValue&)> done) {
    auto request = [this, method, uri, line, column, done = std::move(done)] {
        Request(method,
                {{"textDocument", TextDocument(uri)},
                 {"position", LuxUI::JsonObject{{"line", line - 1}, {"character", column - 1}}}},
                done);
    };
    if (ready_)
        request();
    else if (readyRequests_.size() < 8)
        readyRequests_.push_back(std::move(request));
}

void CSharpLanguageClient::Complete(const LuxUI::String& uri, int line, int column,
                                    std::function<void(const LuxUI::JsonValue&)> done) {
    PositionRequest("textDocument/completion", uri, line, column, std::move(done));
}

void CSharpLanguageClient::Definition(const LuxUI::String& uri, int line, int column,
                                      std::function<void(const LuxUI::JsonValue&)> done) {
    PositionRequest("textDocument/definition", uri, line, column, std::move(done));
}
} // namespace LuxEditor
