#include <LuxEditor/Scripting/ToolProtocol.hpp>

namespace LuxEditor {
ToolProtocol::ToolProtocol(LuxUI::Object* owner) : owner_(owner) {
    LuxUI::Object::connect(&process_, &LuxUI::Process::started, owner_, [this] {
        if (started)
            started();
    });
    LuxUI::Object::connect(&process_, &LuxUI::Process::readyReadStandardOutput, owner_,
                           [this] { Read(); });
    LuxUI::Object::connect(&process_, &LuxUI::Process::readyReadStandardError, owner_, [this] {
        const auto text = LuxUI::String::fromUtf8(process_.readAllStandardError());
        if (output)
            output(text);
    });
    LuxUI::Object::connect(&process_, &LuxUI::Process::errorOccurred, owner_, [this](auto) {
        if (failed)
            failed(process_.errorString());
    });
    LuxUI::Object::connect(
        &process_, qOverload<int, LuxUI::Process::ExitStatus>(&LuxUI::Process::finished), owner_,
        [this](int code, auto) {
            Read();
            if (failed)
                failed(LUX_UI_TEXT("도구 프로세스가 종료됐습니다 (%1).").arg(code));
        });
}

ToolProtocol::~ToolProtocol() {
    Stop();
}

void ToolProtocol::Start(const LuxUI::String& program, const LuxUI::StringList& arguments,
                         const LuxUI::String& directory,
                         const LuxUI::ProcessEnvironment& environment) {
    Stop();
    buffer_.clear();
    length_ = -1;
    process_.setWorkingDirectory(directory);
    process_.setProcessEnvironment(environment);
    process_.start(program, arguments);
}

void ToolProtocol::Stop() {
    const LuxUI::SignalBlocker blocker(&process_);
    if (process_.state() != LuxUI::Process::NotRunning) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    buffer_.clear();
    length_ = -1;
}

bool ToolProtocol::Running() const {
    return process_.state() != LuxUI::Process::NotRunning;
}

void ToolProtocol::Send(const LuxUI::JsonObject& message) {
    const auto body = LuxUI::JsonDocument(message).toJson(LuxUI::JsonDocument::Compact);
    process_.write("Content-Length: " + LuxUI::ByteArray::number(body.size()) + "\r\n\r\n" + body);
}

void ToolProtocol::Read() {
    buffer_ += process_.readAllStandardOutput();
    constexpr qint64 limit = 16 * 1024 * 1024;
    while (!buffer_.isEmpty()) {
        if (length_ < 0) {
            const auto end = buffer_.indexOf("\r\n\r\n");
            if (end < 0 && buffer_.size() <= 8192)
                return;
            bool valid = false;
            if (end >= 0 && end <= 8192)
                for (const auto& header : buffer_.left(end).split('\n'))
                    if (header.trimmed().toLower().startsWith("content-length:")) {
                        length_ = header.mid(header.indexOf(':') + 1).trimmed().toLongLong(&valid);
                        break;
                    }
            if (!valid || length_ < 0 || length_ > limit) {
                Stop();
                if (failed)
                    failed(LUX_UI_TEXT("도구의 메시지 프레임을 읽지 못했습니다."));
                return;
            }
            buffer_.remove(0, end + 4);
        }
        if (buffer_.size() < length_)
            return;
        const auto body = buffer_.left(length_);
        buffer_.remove(0, length_);
        length_ = -1;
        LuxUI::JsonParseError error;
        const auto document = LuxUI::JsonDocument::fromJson(body, &error);
        if (error.error != LuxUI::JsonParseError::NoError || !document.isObject()) {
            Stop();
            if (failed)
                failed(LUX_UI_TEXT("도구의 JSON 메시지를 읽지 못했습니다."));
            return;
        }
        if (received)
            received(document.object());
    }
}

LuxUI::String FindScriptTool(const char* environmentName, const char* setting,
                             const char* executable) {
    auto path = qEnvironmentVariable(environmentName);
    if (path.isEmpty())
        path = LuxUI::Settings().value(LuxUI::String::fromLatin1(setting)).toString();
    if (!path.isEmpty())
        return path;
    auto name = LuxUI::String::fromLatin1(executable);
#ifdef _WIN32
    name += LUX_UI_TEXT(".exe");
#endif
    const auto app = LuxUI::CoreApplication::applicationDirPath();
    for (const auto& directory :
         {app + LUX_UI_TEXT("/tools"), app + LUX_UI_TEXT("/../../.deps/script-tools"),
          app + LUX_UI_TEXT("/../../.deps/script-tools/debugger/netcoredbg")}) {
        const auto candidate = LuxUI::Dir(directory).filePath(name);
        if (LuxUI::FileInfo::exists(candidate))
            return candidate;
    }
    return LuxUI::StandardPaths::findExecutable(name);
}
} // namespace LuxEditor
