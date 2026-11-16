#include <LuxEditor/Project/EditorProject.hpp>
#include <LuxEditor/Project/PathEncoding.hpp>
#include <LuxEditor/Scripting/CSharpLanguageClient.hpp>
#include <LuxEditor/Scripting/ScriptDebugger.hpp>
#include <LuxUI/CodeEditor.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool condition, const char* error) {
    if (!condition)
        throw std::runtime_error(error);
}

void Wait(LuxUI::Application& app, const std::function<bool()>& done, const char* error,
          int seconds = 25) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        app.processEvents();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(done(), error);
}

void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path);
    file << text;
    Check(bool(file), "Fixture write failed");
}
} // namespace

int main(int argc, char** argv) {
    LuxUI::Application app(argc, argv);
    if (argc != 4)
        return 2;
    try {
        LuxUI::TemporaryDir directory;
        const auto root =
            LuxEditor::PathFromUtf8(directory.path().toUtf8().toStdString()) / "CSharp Project";
        std::string error;
        auto project = LuxEditor::EditorProject::Create(root, "LanguageTools",
                                                        LuxEditor::ProjectTemplate::Empty, error);
        Check(project.has_value(), error.c_str());
        const auto file = root / "Scripts/Mover.cs", helper = root / "Scripts/Helper.cs";
        const std::string text =
            "using LuxEngine;\nnamespace Game {\npublic class Mover : EntityScript {\n"
            "    public void Test() {\n        Helper.Compute();\n    }\n}\n}\n";
        Write(file, text);
        Write(helper, "namespace Game {\npublic static class Helper {\n    public static int "
                      "Compute() { return 7; }\n}\n}\n");
        LuxUI::Object owner;
        LuxEditor::CSharpLanguageClient language(&owner);
        LuxUI::String lastStatus;
        language.status = [&](const auto& text) { lastStatus = text; };
        language.Start(LuxUI::String::fromUtf8(LuxEditor::PathUtf8(root)));
        const auto uri =
            LuxUI::Url::fromLocalFile(LuxUI::String::fromUtf8(LuxEditor::PathUtf8(file)))
                .toString();
        language.Update(uri, LuxUI::String::fromUtf8(text), 1);
        Wait(app, [&] { return language.Ready(); }, "Language server initialization timed out");
        bool completed = false, containsCompute = false;
        language.Complete(uri, 5, 16, [&](const auto& result) {
            const auto items =
                result.isArray() ? result.toArray() : result.toObject().value("items").toArray();
            for (const auto& item : items)
                containsCompute |=
                    item.toObject().value("label").toString() == LUX_UI_TEXT("Compute");
            completed = true;
        });
        Wait(app, [&] { return completed; }, "Semantic completion timed out");
        if (!containsCompute)
            throw std::runtime_error("Cross-file member completion missing: " +
                                     lastStatus.toStdString());
        bool defined = false;
        LuxUI::JsonValue definition;
        language.Definition(uri, 5, 10, [&](const auto& value) {
            definition = value;
            defined = true;
        });
        Wait(app, [&] { return defined; }, "Definition request timed out");
        const auto location =
            definition.isArray() ? definition.toArray()[0].toObject() : definition.toObject();
        Check(location.value("uri")
                  .toString(location.value("targetUri").toString())
                  .contains(LUX_UI_TEXT("Helper.cs")),
              "Semantic definition did not resolve to another source file");
        language.Stop();

        std::ifstream source(argv[3]);
        std::string line;
        int breakpointLine = 0, currentLine = 0;
        while (std::getline(source, line)) {
            ++currentLine;
            if (line.find("LuxEditor debugger integration breakpoint") != std::string::npos)
                breakpointLine = currentLine;
        }
        Check(breakpointLine > 0, "Breakpoint source marker missing");
        LuxUI::Process target;
        auto environment = LuxUI::ProcessEnvironment::systemEnvironment();
        environment.insert(LUX_UI_TEXT("LUX_SCRIPT_DEBUG_WAIT"), LUX_UI_TEXT("1"));
        target.setProcessEnvironment(environment);
        target.setProcessChannelMode(LuxUI::Process::MergedChannels);
        target.start(LuxUI::String::fromLocal8Bit(argv[1]),
                     {LuxUI::String::fromLocal8Bit(argv[2])});
        LuxUI::ByteArray output;
        Wait(
            app,
            [&] {
                output += target.readAllStandardOutput();
                return output.contains("LUX_SCRIPT_DEBUG_READY");
            },
            "Native script host did not wait for debugger");
        LuxEditor::ScriptDebugger debugger(&owner);
        bool verified = false;
        debugger.verified = [&](const auto&, const auto& points) {
            verified = !points.empty() && points.front().verified;
        };
        int stops = 0;
        LuxUI::String debugError;
        debugger.status = [&](const auto& text) { debugError = text; };
        debugger.event = [&](const auto& event) {
            if (event.value("event") == "stopped")
                ++stops;
        };
        debugger.SetBreakpoints(LuxUI::String::fromLocal8Bit(argv[3]),
                                {{breakpointLine, LUX_UI_TEXT("Counter == 2"), false}});
        Check(debugger.Attach(target.processId(), directory.path()), "Debugger did not start");
        Wait(app, [&] { return debugger.Paused(); }, "Native host breakpoint was not hit");
        Check(verified && stops == 1, "Breakpoint was not verified by the real debugger");
        bool framesReady = false;
        LuxUI::JsonArray frames;
        debugger.Request("stackTrace", {{"threadId", debugger.ThreadId()}}, [&](const auto& body) {
            frames = body.value("stackFrames").toArray();
            framesReady = true;
        });
        Wait(app, [&] { return framesReady; }, "Call stack request timed out");
        Check(!frames.isEmpty() &&
                  frames[0].toObject().value("name").toString().contains(LUX_UI_TEXT("DebugProbe")),
              "Managed call stack missing");
        const int frame = frames[0].toObject().value("id").toInt();
        bool evaluated = false;
        LuxUI::String value;
        debugger.Request("evaluate",
                         {{"expression", "Counter"}, {"frameId", frame}, {"context", "watch"}},
                         [&](const auto& body) {
                             value = body.value("result").toString();
                             evaluated = true;
                         });
        Wait(app, [&] { return evaluated; }, "Variable evaluation timed out");
        Check(value == LUX_UI_TEXT("2"), "Conditional breakpoint stopped at the wrong value");
        bool scopesReady = false;
        debugger.Request("scopes", {{"frameId", frame}}, [&](const auto& body) {
            scopesReady = !body.value("scopes").toArray().isEmpty();
        });
        Wait(app, [&] { return scopesReady; }, "Variable scopes missing");
        debugger.Control("next");
        Wait(app, [&] { return stops >= 2; }, "Step over did not stop again");
        debugger.Control("continue");
        Wait(app, [&] { return !debugger.Paused(); }, "Continue did not resume the target");
        debugger.Stop();
        target.kill();
        target.waitForFinished(1000);
        std::cout << "Real C# completion/definition and native-host breakpoints, conditions, "
                     "stack, evaluation and stepping passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
