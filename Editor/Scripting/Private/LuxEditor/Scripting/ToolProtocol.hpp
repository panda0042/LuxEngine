#pragma once

#include <LuxUI/Widgets.hpp>
#include <functional>

namespace LuxEditor {
// LSP and DAP both use UTF-8 JSON messages framed by a byte Content-Length.
class ToolProtocol final {
public:
    explicit ToolProtocol(LuxUI::Object* owner);
    ~ToolProtocol();
    void Start(const LuxUI::String& program, const LuxUI::StringList& arguments,
               const LuxUI::String& directory, const LuxUI::ProcessEnvironment& environment);
    void Stop();
    bool Running() const;
    void Send(const LuxUI::JsonObject& message);
    std::function<void()> started;
    std::function<void(const LuxUI::JsonObject&)> received;
    std::function<void(const LuxUI::String&)> failed;
    std::function<void(const LuxUI::String&)> output;

private:
    void Read();
    LuxUI::Object* owner_;
    LuxUI::Process process_;
    LuxUI::ByteArray buffer_;
    qint64 length_ = -1;
};

LuxUI::String FindScriptTool(const char* environmentName, const char* setting,
                             const char* executable);
} // namespace LuxEditor
