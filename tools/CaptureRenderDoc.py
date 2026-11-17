#!/usr/bin/env python3
"""Validate a bounded GPU workload, then capture/replay it with RenderDoc on Linux."""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile


TRIGGER = r'''
#include "renderdoc_app.h"
#include <dlfcn.h>
#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <cstring>
static void RecordExit(int status, void*) {
    const char* path = std::getenv("LUX_RENDERDOC_EXIT_STATUS");
    if (!path) return;
    if (FILE* file = std::fopen(path, "w")) {
        std::fprintf(file, "%d", status);
        std::fclose(file);
    }
}
__attribute__((constructor)) static void Capture() {
    char executable[4096] = {};
    readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    const char* target = std::getenv("LUX_RENDERDOC_TARGET");
    if (!target || std::strcmp(executable, target) != 0) return;
    on_exit(RecordExit, nullptr);
    auto* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (!module) return;
    auto get = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(module, "RENDERDOC_GetAPI"));
    RENDERDOC_API_1_1_2* api = nullptr;
    if (!get || !get(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&api))) return;
    api->MaskOverlayBits(0, 0);
    api->TriggerCapture();
}
'''


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "out")
    parser.add_argument("--exe", type=Path, help="Defaults to LuxForgeBackendTests")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--icd", type=Path, help="Optional Vulkan driver manifest")
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("args", nargs=argparse.REMAINDER, help="Bounded workload arguments")
    options = parser.parse_args()
    if options.timeout <= 0:
        parser.error("--timeout must be positive")
    executable = (options.exe or options.build_dir / "bin/LuxForgeBackendTests").resolve()
    args = options.args
    if args[:1] == ["--"]:
        args = args[1:]
    if not args and not options.exe:
        args = ["scene3d"]
    if not executable.is_file() or not os.environ.get("DISPLAY"):
        parser.error("An existing executable and X11 DISPLAY are required")
    command = shutil.which("renderdoccmd")
    compiler = shutil.which("c++")
    if not command or not compiler:
        parser.error("renderdoccmd and c++ are required")
    output = options.output or Path(tempfile.mkdtemp(prefix="lux-renderdoc-"))
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if list(output.glob("*.rdc")):
        parser.error("Use an output directory without existing captures")
    data = output / "data"
    data.mkdir(exist_ok=True)
    settings = output / "vk_layer_settings.txt"
    settings.write_text(
        "khronos_validation.validate_core = true\n"
        "khronos_validation.validate_sync = true\n"
        "khronos_validation.syncval_submit_time_validation = true\n"
        "khronos_validation.report_flags = error,warn,info\n"
        "khronos_validation.debug_action = VK_DBG_LAYER_ACTION_LOG_MSG\n"
        "khronos_validation.log_filename = stdout\n"
    )
    env = os.environ | {
        "SDL_VIDEODRIVER": "x11",
        "SDL_AUDIO_DRIVER": "dummy",
        # Ignore stale per-user RenderDoc layer manifests without changing the account.
        "XDG_DATA_HOME": str(data),
        "VK_LAYER_SETTINGS_PATH": str(output),
        "VK_INSTANCE_LAYERS": "VK_LAYER_KHRONOS_validation",
    }
    env.pop("VK_LOADER_LAYERS_DISABLE", None)
    env.pop("LD_PRELOAD", None)
    if options.icd:
        env["VK_DRIVER_FILES"] = str(options.icd.resolve())

    def run(label, argv, environment):
        log = output / f"{label}.log"
        with log.open("w") as stream:
            process = subprocess.Popen(
                argv, cwd=executable.parent, env=environment, stdout=stream,
                stderr=subprocess.STDOUT, start_new_session=True,
            )
            try:
                returncode = process.wait(timeout=options.timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                raise RuntimeError(f"{label} timed out; see {log}") from None
        text = log.read_text(errors="replace")
        if returncode or re.search(
            r"Validation Error:|SYNC-HAZARD-|^FAIL(?:ED)?[: ]", text, re.MULTILINE
        ):
            raise RuntimeError(f"{label} failed; see {log}")
        return text

    baseline = run("validation", [str(executable), *args], env)
    if "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT" not in baseline:
        raise RuntimeError(f"Synchronization validation was not confirmed; see {output}")
    source = output / "trigger.cpp"
    source.write_text(TRIGGER)
    trigger = output / "trigger.so"
    header = root / "Engine/ThirdParty/TheForge/Common_3/Graphics/ThirdParty/OpenSource/renderdoc"
    subprocess.run(
        [compiler, "-std=c++17", "-shared", "-fPIC", "-I", str(header), str(source),
         "-ldl", "-o", str(trigger)], check=True,
    )
    # Validation has already run without interception. RenderDoc 1.42's presentation
    # emulation and an additional live validation layer disagree on image layouts.
    # Keep these phases separate; never suppress failures in the validation phase.
    capture_env = env | {
        "VK_LOADER_LAYERS_DISABLE": "VK_LAYER_KHRONOS_validation",
        "VK_INSTANCE_LAYERS": "",
        "LD_PRELOAD": str(trigger),
        "LUX_RENDERDOC_TARGET": str(executable),
        "LUX_RENDERDOC_EXIT_STATUS": str(output / "capture-exit.txt"),
    }
    # renderdoccmd -w does not propagate the application's exit code. A child
    # exit callback records it independently. A crash leaves no successful status.
    exit_status = output / "capture-exit.txt"
    exit_status.unlink(missing_ok=True)
    run("capture", [command, "capture", "-w", "-c", str(output / "frame"),
                    str(executable), *args], capture_env)
    if not exit_status.is_file() or exit_status.read_text().strip() != "0":
        raise RuntimeError(f"Captured application failed; see {output / 'capture.log'}")
    captures = sorted(output.glob("*.rdc"))
    if not captures:
        raise RuntimeError(f"No frame was captured; see {output / 'capture.log'}")
    replay_env = capture_env.copy()
    replay_env.pop("LD_PRELOAD")
    for index, capture in enumerate(captures):
        run(f"replay-{index}", [command, "replay", "-l", "1", str(capture)], replay_env)
    (output / "result.json").write_text(json.dumps({
        "executable": str(executable), "args": args, "validation": "core+sync, unhooked",
        "capture_validation_layer": "disabled only during RenderDoc interception",
        "captures": [str(path) for path in captures], "replay": "passed",
    }, indent=2) + "\n")
    print(f"Validation, capture and replay passed: {output}")


if __name__ == "__main__":
    main()
