#pragma once
#include <LuxEditor/Scripting/ToolProtocol.hpp>
#include <map>
#include <vector>

namespace LuxEditor {
class CSharpLanguageClient final {
public:
    explicit CSharpLanguageClient(LuxUI::Object* owner);
    ~CSharpLanguageClient();
    void Start(const LuxUI::String& root);
    void Stop();
    void Update(const LuxUI::String& uri, const LuxUI::String& text, int version);
    void Close(const LuxUI::String& uri);
    void Complete(const LuxUI::String& uri, int line, int column,
                  std::function<void(const LuxUI::JsonValue&)> done);
    void Definition(const LuxUI::String& uri, int line, int column,
                    std::function<void(const LuxUI::JsonValue&)> done);
    bool Ready() const;
    std::function<void(const LuxUI::String&)> status;
    std::function<void(const LuxUI::JsonObject&)> diagnostics;

private:
    void Receive(const LuxUI::JsonObject& message);
    void Notify(const char* method, const LuxUI::JsonObject& parameters);
    void Request(const char* method, const LuxUI::JsonObject& parameters,
                 std::function<void(const LuxUI::JsonValue&)> done);
    void PositionRequest(const char* method, const LuxUI::String& uri, int line, int column,
                         std::function<void(const LuxUI::JsonValue&)> done);

    struct Document {
        LuxUI::String text;
        int version = 1;
        bool open = false;
    };

    ToolProtocol transport_;
    LuxUI::String root_;
    std::map<LuxUI::String, Document> documents_;
    std::map<int, std::function<void(const LuxUI::JsonValue&)>> pending_;
    std::vector<std::function<void()>> readyRequests_;
    int nextId_ = 1;
    bool ready_ = false;
};
} // namespace LuxEditor
