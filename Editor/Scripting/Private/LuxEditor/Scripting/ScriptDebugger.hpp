#pragma once
#include <LuxEditor/Scripting/ToolProtocol.hpp>
#include <LuxUI/CodeEditor.hpp>
#include <map>

namespace LuxEditor {
class ScriptDebugger final {
public:
    explicit ScriptDebugger(LuxUI::Object* owner);
    ~ScriptDebugger();
    bool Attach(qint64 processId, const LuxUI::String& root);
    bool Launch(const LuxUI::String& program, const LuxUI::StringList& arguments,
                const LuxUI::String& root);
    void Stop();
    bool Active() const;
    bool Paused() const;
    int ThreadId() const;
    void SetBreakpoints(const LuxUI::String& path,
                        const std::vector<LuxUI::CodeBreakpoint>& points);
    void Request(const char* command, const LuxUI::JsonObject& arguments,
                 std::function<void(const LuxUI::JsonObject&)> done = {});
    void Control(const char* command);
    std::function<void(const LuxUI::JsonObject&)> event;
    std::function<void(const LuxUI::String&)> status;
    std::function<void(const LuxUI::String&, const std::vector<LuxUI::CodeBreakpoint>&)> verified;

private:
    bool Start(const LuxUI::String& root, const char* command, const LuxUI::JsonObject& arguments);
    void Receive(const LuxUI::JsonObject& message);
    void SendBreakpoints(const LuxUI::String& path, std::function<void()> done = {});
    ToolProtocol transport_;
    std::map<int, std::function<void(const LuxUI::JsonObject&)>> pending_;
    std::map<LuxUI::String, std::vector<LuxUI::CodeBreakpoint>> breakpoints_;
    std::map<int, std::pair<LuxUI::String, size_t>> breakpointIds_;
    int sequence_ = 1;
    int thread_ = 0;
    bool active_ = false, paused_ = false, configured_ = false;
};
} // namespace LuxEditor
