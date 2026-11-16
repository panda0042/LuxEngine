#include <LuxEditor/Build/BuildOutput.hpp>
#include <LuxEditor/Scripting/ScriptDebugger.hpp>

namespace LuxEditor {
ScriptDebugger::ScriptDebugger(LuxUI::Object* owner) : transport_(owner) {
    transport_.received = [this](const auto& message) { Receive(message); };
    transport_.failed = [this](const auto& error) {
        active_ = paused_ = configured_ = false;
        pending_.clear();
        if (status)
            status(error);
        if (event)
            event({{"event", "terminated"}});
    };
}

ScriptDebugger::~ScriptDebugger() {
    transport_.Stop();
}

bool ScriptDebugger::Active() const {
    return active_;
}

bool ScriptDebugger::Paused() const {
    return paused_;
}

int ScriptDebugger::ThreadId() const {
    return thread_;
}

bool ScriptDebugger::Attach(qint64 processId, const LuxUI::String& root) {
    if (processId <= 0)
        return false;
    return Start(root, "attach", {{"processId", double(processId)}, {"justMyCode", true}});
}

bool ScriptDebugger::Launch(const LuxUI::String& program, const LuxUI::StringList& arguments,
                            const LuxUI::String& root) {
    LuxUI::JsonArray args;
    for (const auto& value : arguments)
        args.append(value);
    return Start(root, "launch",
                 {{"program", program},
                  {"args", args},
                  {"cwd", root},
                  {"stopAtEntry", false},
                  {"justMyCode", true},
                  {"console", "internalConsole"}});
}

bool ScriptDebugger::Start(const LuxUI::String& root, const char* command,
                           const LuxUI::JsonObject& arguments) {
    Stop();
    const auto program = FindScriptTool("LUX_NETCOREDBG", "scripts/debugAdapter", "netcoredbg");
    if (program.isEmpty()) {
        if (status)
            status(LUX_UI_TEXT("netcoredbg 경로를 설정해 주세요."));
        return false;
    }
    active_ = true;
    transport_.started = [this, command, arguments] {
        Request("initialize",
                {{"clientID", "LuxEditor"},
                 {"adapterID", "coreclr"},
                 {"pathFormat", "path"},
                 {"linesStartAt1", true},
                 {"columnsStartAt1", true},
                 {"supportsVariableType", true},
                 {"supportsRunInTerminalRequest", false}},
                [this, command, arguments](const auto&) { Request(command, arguments); });
    };
    transport_.Start(program, {LUX_UI_TEXT("--interpreter=vscode")}, root, Utf8BuildEnvironment());
    return true;
}

void ScriptDebugger::Stop() {
    if (active_ && transport_.Running())
        Request("disconnect", {{"terminateDebuggee", true}});
    transport_.Stop();
    pending_.clear();
    breakpointIds_.clear();
    active_ = paused_ = configured_ = false;
    thread_ = 0;
}

void ScriptDebugger::Request(const char* command, const LuxUI::JsonObject& arguments,
                             std::function<void(const LuxUI::JsonObject&)> done) {
    if (!active_)
        return;
    const int sequence = sequence_++;
    pending_[sequence] = std::move(done);
    transport_.Send({{"seq", sequence},
                     {"type", "request"},
                     {"command", LuxUI::String::fromLatin1(command)},
                     {"arguments", arguments}});
}

void ScriptDebugger::Receive(const LuxUI::JsonObject& message) {
    if (message.value("type") == "response") {
        const auto found = pending_.find(message.value("request_seq").toInt());
        if (found == pending_.end())
            return;
        auto done = std::move(found->second);
        pending_.erase(found);
        if (!message.value("success").toBool()) {
            if (status)
                status(message.value("message").toString());
            // An unsuccessful attach/launch leaves the UI usable and closes the waiting game.
            if (message.value("command") == "attach" || message.value("command") == "launch" ||
                message.value("command") == "initialize") {
                active_ = paused_ = false;
                if (event)
                    event({{"event", "terminated"}});
            } else if (done)
                done({});
        } else if (done)
            done(message.value("body").toObject());
        return;
    }
    if (message.value("type") != "event")
        return;
    const auto name = message.value("event").toString();
    if (name == "initialized") {
        configured_ = true;
        if (breakpoints_.empty())
            Request("configurationDone", {});
        else {
            auto count = std::make_shared<size_t>(breakpoints_.size());
            for (const auto& [path, points] : breakpoints_) {
                Q_UNUSED(points);
                SendBreakpoints(path, [this, count] {
                    if (--*count == 0)
                        Request("configurationDone", {});
                });
            }
        }
    } else if (name == "breakpoint") {
        const auto point = message.value("body").toObject().value("breakpoint").toObject();
        const auto found = breakpointIds_.find(point.value("id").toInt(-1));
        if (found != breakpointIds_.end()) {
            auto& points = breakpoints_[found->second.first];
            if (found->second.second < points.size()) {
                auto& value = points[found->second.second];
                value.verified = point.value("verified").toBool();
                value.line = point.value("line").toInt(value.line);
                if (verified)
                    verified(found->second.first, points);
            }
        }
    } else if (name == "stopped") {
        paused_ = true;
        thread_ = message.value("body").toObject().value("threadId").toInt();
    } else if (name == "continued")
        paused_ = false;
    else if (name == "terminated" || name == "exited")
        active_ = paused_ = false;
    if (event)
        event(message);
}

void ScriptDebugger::SetBreakpoints(const LuxUI::String& path,
                                    const std::vector<LuxUI::CodeBreakpoint>& points) {
    breakpoints_[path] = points;
    if (configured_ && active_)
        SendBreakpoints(path);
}

void ScriptDebugger::SendBreakpoints(const LuxUI::String& path, std::function<void()> done) {
    auto points = breakpoints_[path];
    LuxUI::JsonArray values;
    for (const auto& point : points)
        values.append(LuxUI::JsonObject{{"line", point.line}, {"condition", point.condition}});
    Request(
        "setBreakpoints", {{"source", LuxUI::JsonObject{{"path", path}}}, {"breakpoints", values}},
        [this, path, points, done](const auto& body) mutable {
            const auto responses = body.value("breakpoints").toArray();
            for (size_t i = 0; i < points.size(); ++i)
                points[i].verified = i < size_t(responses.size()) &&
                                     responses[int(i)].toObject().value("verified").toBool();
            for (size_t i = 0; i < points.size() && i < size_t(responses.size()); ++i)
                if (responses[int(i)].toObject().contains("id"))
                    breakpointIds_[responses[int(i)].toObject().value("id").toInt()] = {path, i};
            breakpoints_[path] = points;
            if (verified)
                verified(path, points);
            if (done)
                done();
        });
}

void ScriptDebugger::Control(const char* command) {
    Request(command, {{"threadId", thread_}});
    if (std::string_view(command) != "pause" && paused_) {
        paused_ = false;
        if (event)
            event({{"event", "continued"}});
    }
}
} // namespace LuxEditor
