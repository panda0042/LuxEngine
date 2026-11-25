// GpuDevice running on The Forge backend (LUX_RENDER_BACKEND=forge): frame loop, clear,
// capture, resize and vsync. Exit code 77 = skipped (no display), 0 = pass.
#include <LuxEngine/LuxEngine.hpp>
#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <LuxEngine/Renderer/Water/WaterSurface.hpp>
// Mesh tests drive the render backend directly (SetLighting/DrawIndexed are reserved for
// Renderer3D on GpuDevice).
#include "GpuTestEnvironment.hpp"
#include "RHI/RenderBackend.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <string>
#include <vector>

using namespace LuxEngine;

namespace {
std::string g_StopFile; // stop long runs early when this file exists

bool StopRequested() {
    return !g_StopFile.empty() && std::filesystem::exists(g_StopFile);
}

int g_Checks = 0;
int g_Failures = 0;

void Check(bool condition, const char* what) {
    ++g_Checks;
    if (!condition) {
        ++g_Failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool Begin(Window& window, GpuDevice& gpu) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        window.PollEvents();
        const auto start = gpu.BeginFrame();
        if (start == GpuDevice::FrameStart::Ready)
            return true;
        if (start == GpuDevice::FrameStart::Error) {
            std::printf("  BeginFrame failed: status=%s error=%s\n", ToString(gpu.GetStatus()),
                        gpu.GetLastError().c_str());
            return false;
        }
        SDL_Delay(5);
    }
    uint32 width = 0, height = 0;
    window.GetFramebufferSize(width, height);
    std::printf("  BeginFrame timed out: framebuffer=%ux%u minimized=%d flags=0x%llx "
                "skipped=%llu status=%s error=%s\n",
                width, height, window.IsMinimized(),
                static_cast<unsigned long long>(SDL_GetWindowFlags(window.GetNativeHandle())),
                static_cast<unsigned long long>(gpu.GetCounters().framesSkipped),
                ToString(gpu.GetStatus()), gpu.GetLastError().c_str());
    return false;
}

bool CaptureClear(Window& window, GpuDevice& gpu, float r, float g, float b, GpuFrameCapture& out) {
    return gpu.RequestFrameCapture() && Begin(window, gpu) && gpu.Clear(r, g, b, 1) &&
           gpu.EndFrame() && gpu.ReadFrameCapture(out) &&
           out.rgba.size() == static_cast<size_t>(out.width) * out.height * 4;
}

bool RestoreForCapture(Window& window) {
    auto* native = window.GetNativeHandle();
    if (!native)
        return false;
    const auto start = SDL_GetTicks();
    do {
        window.PollEvents();
        if (SDL_ShowWindow(native) && SDL_RestoreWindow(native) && SDL_RaiseWindow(native) &&
            SDL_SyncWindow(native)) {
            window.PollEvents();
            if (!window.IsMinimized())
                return true;
        }
        SDL_Delay(10);
    } while (SDL_GetTicks() - start < 5000);
    return false;
}

bool Near(int a, int b) {
    return a - b <= 1 && b - a <= 1;
}

bool CentreIs(const GpuFrameCapture& capture, int r, int g, int b) {
    if (capture.rgba.empty())
        return false;
    const uint8* p =
        &capture
             .rgba[(static_cast<size_t>(capture.height / 2) * capture.width + capture.width / 2) *
                   4];
    std::printf("  centre=(%u,%u,%u,%u) expected=(%d,%d,%d,255)\n", p[0], p[1], p[2], p[3], r, g,
                b);
    return Near(p[0], r) && Near(p[1], g) && Near(p[2], b) && p[3] == 255;
}

struct Session {
    Window window;
    GpuDevice gpu;
    bool ok = false;

    explicit Session(uint32 width = 320, uint32 height = 240, GpuDeviceConfig config = {},
                     bool hidden = false) {
        WindowProps props;
        props.width = width;
        props.height = height;
        props.title = "LuxEngine Forge backend verification";
        props.hidden = hidden;
        props.vulkan = false;
        if (!window.Initialize(props))
            return;
        ok = gpu.Initialize(window, config);
        if (!ok)
            std::printf("  init failed: %s\n", gpu.GetLastError().c_str());
    }

    ~Session() {
        gpu.Shutdown();
        window.Shutdown();
    }
};

void Diagnostics() {
    Session first;
    Check(first.ok, "diagnostic device starts");
    if (!first.ok)
        return;
    const auto baseline = first.gpu.GetMemoryStats();
    GpuBuffer buffer;
    Check(buffer.Initialize(&first.gpu, BufferType::Vertex, 4096), "diagnostic buffer created");
    const auto allocated = first.gpu.GetMemoryStats();
    Check(allocated.liveBuffers == baseline.liveBuffers + 1 &&
              allocated.allocationBytes >= baseline.allocationBytes + 4096,
          "GPU buffer payload and live count are reported");
    const std::array<uint8, 64> payload{};
    Check(buffer.SetData(payload.data(), 64), "diagnostic upload outside a frame");
    GpuTexture texture;
    Check(Begin(first.window, first.gpu) && first.gpu.Clear(0, 0, 0, 1) &&
              buffer.SetData(payload.data(), 64) && buffer.SetData(payload.data(), 16, 64) &&
              !buffer.SetData(payload.data(), 64, 4095) &&
              texture.Initialize(&first.gpu, 4, 4, payload.data(), TextureFilter::Linear,
                                 TextureColorSpace::Linear, {.mipmaps = true}),
          "upload diagnostic frame records buffer updates and a mipmapped texture");
    const auto activeUploads = first.gpu.GetFrameStats();
    Check(activeUploads.uploadBytes == 64 + 16 + 64 + 16 + 4 && activeUploads.submissions == 0 &&
              activeUploads.drawCalls == 0,
          "active frame reports uploads without waiting for a draw or submission");
    Check(first.gpu.EndFrame(), "upload diagnostic frame submits successfully");
    const auto uploads = first.gpu.GetFrameStats();
    Check(uploads.uploadBytes == 64 + 16 + 64 + 16 + 4,
          "upload bytes count successful buffer ranges and all texture mips in the current frame");
    Check(uploads.uploadCpuMs > 0 && uploads.uploadWaitMs >= 0 &&
              uploads.uploadWaitMs <= uploads.uploadCpuMs &&
              uploads.uploadWaitMs <= uploads.cpuWaitMs,
          "upload waits belong to both CPU upload time and total GPU wait time");
    Check(buffer.SetData(payload.data(), 64) && Begin(first.window, first.gpu) &&
              first.gpu.Clear(0, 0, 0, 1) && first.gpu.EndFrame(),
          "upload metrics test submits a frame without uploads");
    const auto idleUploads = first.gpu.GetFrameStats();
    Check(idleUploads.uploadBytes == 0 && idleUploads.uploadCpuMs == 0 &&
              idleUploads.uploadWaitMs == 0 && idleUploads.cpuWaitMs >= 0,
          "upload metrics are reset per frame and exclude out-of-frame transfers");
    texture.Release();
    buffer.Release();
    Check(first.gpu.GetMemoryStats().pendingDestructions > 0,
          "deferred buffer release remains visible");
    Check(first.gpu.WaitIdle(), "diagnostic buffers drained");
    const auto released = first.gpu.GetMemoryStats();
    Check(released.liveBuffers == baseline.liveBuffers &&
              released.allocationBytes == baseline.allocationBytes &&
              released.pendingDestructions == 0,
          "buffer memory returns to its baseline after GPU completion");
#if defined(_WIN32)
    const auto library = std::string(SDL_GetBasePath()) + "LuxForge.dll";
#elif defined(__APPLE__)
    const auto library = std::string(SDL_GetBasePath()) + "libLuxForge.dylib";
#else
    const auto library = std::string(SDL_GetBasePath()) + "libLuxForge.so";
#endif
    auto* module = SDL_LoadObject(library.c_str());
    auto emit =
        module
            ? reinterpret_cast<void (*)(int)>(SDL_LoadFunction(module, "LuxForgeTestValidationLog"))
            : nullptr;
    Check(emit != nullptr, "test build exposes the Forge diagnostic log probe");
    if (emit) {
        const auto before = first.gpu.GetCounters();
        emit(-1);
        Check(first.gpu.GetCounters().validationErrors == before.validationErrors,
              "unrelated application errors do not count as validation errors");
        emit(0);
        emit(1);
        const auto after = first.gpu.GetCounters();
        Check(after.validationErrors == before.validationErrors + 1 &&
                  after.validationWarnings == before.validationWarnings + 1,
              "Forge error and warning callbacks reach the engine counters");
        Session second;
        Check(second.ok && second.gpu.GetCounters().validationErrors == 0 &&
                  second.gpu.GetCounters().validationWarnings == 0,
              "a new device does not inherit earlier validation history");
    }
    if (module)
        SDL_UnloadObject(module);
}

void Basics() {
    std::printf("[basics]\n");
    Session s;
    Check(s.ok, "Forge backend starts");
    if (!s.ok)
        return;
    const auto& info = s.gpu.GetInfo();
    std::printf("  device: %s (%s)\n", info.deviceName.c_str(), info.deviceType.c_str());
    Check(info.deviceType.find("Forge") != std::string::npos, "device reports The Forge");
    Check(!info.deviceName.empty(), "device has a name");
    for (int frame = 0; frame < 5; ++frame)
        Check(Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1) && s.gpu.EndFrame(),
              "warm-up frame");
    GpuFrameCapture capture;
    Check(CaptureClear(s.window, s.gpu, .2f, .4f, .6f, capture), "clear frame captured");
    Check(capture.width == 320 && capture.height == 240, "capture has window size");
    Check(CentreIs(capture, 51, 102, 153), "clear colour reaches the screen");
    Check(CaptureClear(s.window, s.gpu, 1, .5f, 0, capture), "second colour captured");
    Check(CentreIs(capture, 255, 128, 0), "second clear colour");
    // Every pixel, not just the centre.
    bool uniform = true;
    for (size_t i = 0; i < capture.rgba.size(); i += 4)
        uniform &= Near(capture.rgba[i], 255) && Near(capture.rgba[i + 1], 128) &&
                   Near(capture.rgba[i + 2], 0) && capture.rgba[i + 3] == 255;
    Check(uniform, "whole frame has the clear colour");
}

void Misuse() {
    std::printf("[misuse]\n");
    GpuDevice none;
    Check(!none.Clear(0, 0, 0, 1), "uninitialized clear rejected");
    Check(none.BeginFrame() == GpuDevice::FrameStart::Error, "uninitialized begin rejected");

    Session s;
    if (!s.ok)
        return Check(false, "session for misuse");
    Check(!s.gpu.Clear(0, 0, 0, 1), "clear outside a frame rejected");
    Check(!s.gpu.EndFrame(), "end without begin rejected");
    GpuFrameCapture capture;
    Check(!s.gpu.ReadFrameCapture(capture), "read without capture rejected");
    Check(Begin(s.window, s.gpu), "frame begins");
    Check(s.gpu.BeginFrame() == GpuDevice::FrameStart::Error, "second begin rejected");
    Check(!s.gpu.SetVSync(false), "vsync change inside a frame rejected");
    Check(s.gpu.Clear(0, 0, 1, 1) && s.gpu.EndFrame(), "frame still completes");
    Check(s.gpu.IsReady() && s.gpu.GetStatus() == GpuStatus::Ready,
          "misuse keeps the device usable");
    Check(CaptureClear(s.window, s.gpu, 0, 1, 0, capture) && CentreIs(capture, 0, 255, 0),
          "rendering after misuse");
    GpuBuffer buffer, invalidBuffer;
    GpuTexture texture;
    Mesh3D mesh;
    const std::array<Vec3, 3> positions{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}};
    const std::array<uint32, 3> indices{0, 1, 2}, invalidIndices{0, 1, 5};
    Check(buffer.Initialize(&s.gpu, BufferType::Vertex, 64) && texture.Initialize(&s.gpu, 4, 4) &&
              mesh.Upload(&s.gpu, positions, indices),
          "resource replacements start from valid GPU objects");
    const auto validResources = s.gpu.GetMemoryStats();
    Check(!invalidBuffer.Initialize(&s.gpu, static_cast<BufferType>(99), 64) &&
              !invalidBuffer.IsValid(),
          "invalid buffer types do not create a vertex buffer by default");
    Check(!buffer.Initialize(&s.gpu, BufferType::Vertex, 0) &&
              !buffer.Initialize(&s.gpu, static_cast<BufferType>(99), 64) && buffer.IsValid() &&
              buffer.GetSize() == 64 && buffer.GetType() == BufferType::Vertex,
          "failed buffer replacement keeps its previous handle and type");
    Check(!texture.Initialize(&s.gpu, 0, 4) && !texture.Initialize(nullptr, 4, 4) &&
              texture.IsValid() && texture.GetWidth() == 4 && texture.GetHeight() == 4,
          "failed texture replacement keeps its image and dimensions");
    Check(!mesh.Upload(&s.gpu, positions, invalidIndices) &&
              !mesh.Upload(nullptr, positions, indices) && mesh.IsValid(),
          "failed mesh replacement keeps its previous geometry");
    const auto afterFailedReplacements = s.gpu.GetMemoryStats();
    Check(afterFailedReplacements.liveBuffers == validResources.liveBuffers &&
              afterFailedReplacements.liveTextures == validResources.liveTextures &&
              afterFailedReplacements.allocationBytes == validResources.allocationBytes,
          "failed replacements preserve live allocations without allocating extras");
    Check(buffer.Initialize(&s.gpu, BufferType::Index, 12) && buffer.GetSize() == 12 &&
              buffer.GetType() == BufferType::Index && texture.Initialize(&s.gpu, 8, 4) &&
              texture.GetWidth() == 8 && mesh.Upload(&s.gpu, positions, indices),
          "valid replacements still update their resource handles");
    Window secondWindow;
    Check(secondWindow.Initialize({.title = "Second Forge viewport", .width = 96, .height = 64}),
          "second window initialized");
    GpuDevice second;
    Check(second.Initialize(secondWindow), "second Forge viewport initialized");
    Check(CaptureClear(secondWindow, second, 0, 0, 1, capture) && CentreIs(capture, 0, 0, 255),
          "second viewport renders independently");
    Check(CaptureClear(s.window, s.gpu, 1, 0, 0, capture) && CentreIs(capture, 255, 0, 0),
          "first device remains usable alongside second viewport");
    second.Shutdown();
    Check(second.Initialize(secondWindow) && Begin(secondWindow, second),
          "frame can be abandoned during viewport shutdown");
    second.Shutdown();
    Check(CaptureClear(s.window, s.gpu, 0, 1, 0, capture) && CentreIs(capture, 0, 255, 0),
          "destroying another viewport preserves the shared renderer");
}

void Ownership() {
    std::printf("[ownership]\n");
    Session first(128, 128, {}, true), second(128, 128, {}, true);
    if (!first.ok || !second.ok)
        return Check(false, "sessions for mesh ownership");
    const Vec3 outside[] = {{100, 100, 0}, {101, 100, 0}, {100, 101, 0}};
    const Vec3 centered[] = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    const uint32 indices[] = {0, 1, 2};
    Mesh3D foreign, local;
    if (!foreign.Upload(&first.gpu, outside, indices) ||
        !local.Upload(&second.gpu, centered, indices))
        return Check(false, "meshes uploaded to independent devices");
    Renderer3D::Context context(&second.gpu);
    PerspectiveCamera camera(ToRadians(60), 1, .1f, 100);
    camera.SetPosition({0, 0, 4});
    camera.SetTarget({0, 0, 0});
    for (const auto mode : {Render3DMode::Legacy, Render3DMode::Pbr}) {
        Render3DSettings settings;
        settings.mode = mode;
        settings.shadows = false;
        settings.showSkybox = false;
        settings.imageBasedLighting = false;
        if (!Begin(second.window, second.gpu))
            return Check(false, "foreign mesh frame begins");
        Check(second.gpu.RequestFrameCapture() && second.gpu.Clear(0, 0, 1, 1),
              "foreign mesh frame prepared");
        context.BeginScene(camera, settings, 1);
        context.DrawMesh(foreign, Mat4{}, Material3D{});
        Check(context.GetStats().meshCount == 0 && context.GetStats().drawCalls == 0,
              "foreign mesh is rejected before recording");
        context.EndScene();
        GpuFrameCapture capture;
        Check(second.gpu.EndFrame() && second.gpu.ReadFrameCapture(capture) &&
                  CentreIs(capture, 0, 0, 255),
              "foreign mesh cannot alias the current device's buffers");
        if (!Begin(second.window, second.gpu))
            return Check(false, "local mesh frame begins");
        Check(second.gpu.RequestFrameCapture() && second.gpu.Clear(0, 0, 1, 1),
              "local mesh frame prepared");
        context.BeginScene(camera, settings, 1);
        context.DrawMesh(local, Mat4{}, Material3D{});
        Check(context.GetStats().meshCount == 1, "local mesh remains accepted");
        context.EndScene();
        const bool captured = second.gpu.EndFrame() && second.gpu.ReadFrameCapture(capture);
        Check(captured, "local mesh frame captured");
        if (captured) {
            const auto pixel =
                (static_cast<size_t>(capture.height / 2) * capture.width + capture.width / 2) * 4;
            Check(capture.rgba[pixel] > 16 && capture.rgba[pixel + 1] > 16,
                  "local mesh still reaches the screen");
        }
    }
}

void Resizing() {
    std::printf("[resize]\n");
    Session s;
    if (!s.ok)
        return Check(false, "session for resize");
    const uint32 sizes[][2] = {{400, 300}, {64, 48}, {1, 1}, {800, 600}, {333, 177}, {320, 240}};
    for (const auto& size : sizes) {
        Check(s.window.SetSize(size[0], size[1]), "window resized");
        s.window.PollEvents();
        uint32 fw = 0, fh = 0;
        s.window.GetFramebufferSize(fw, fh);
        GpuFrameCapture capture;
        // No NotifyResize on purpose: the backend must follow the window by itself.
        Check(CaptureClear(s.window, s.gpu, .1f, .9f, .3f, capture), "frame after resize");
        std::printf("  window %ux%u framebuffer %ux%u capture %ux%u\n", size[0], size[1], fw, fh,
                    capture.width, capture.height);
        Check(capture.width == fw && capture.height == fh, "swap chain follows the framebuffer");
        Check(CentreIs(capture, 26, 230, 77), "clear after resize");
    }
    if (std::string(SDL_GetCurrentVideoDriver()) == "wayland") {
        std::printf("  minimize/restore subcase not run: Wayland requires compositor/user "
                    "activation to restore a minimized window\n");
        return;
    }
    SDL_MinimizeWindow(s.window.GetNativeHandle());
    for (int i = 0; i < 50 && !s.window.IsMinimized(); ++i) {
        s.window.PollEvents();
        SDL_Delay(10);
    }
    if (s.window.IsMinimized()) {
        Check(s.gpu.BeginFrame() == GpuDevice::FrameStart::Skipped, "minimized frame skipped");
        Check(RestoreForCapture(s.window), "minimized window restore completes");
        GpuFrameCapture capture;
        Check(CaptureClear(s.window, s.gpu, .5f, .5f, .5f, capture) &&
                  CentreIs(capture, 128, 128, 128),
              "rendering after restore");
    } else {
        std::printf("  (window manager did not minimize; skipped)\n");
    }
}

void Lifecycle() {
    std::printf("[lifecycle]\n");
    Window window;
    WindowProps props;
    props.width = 200;
    props.height = 150;
    props.vulkan = false;
    if (!window.Initialize(props))
        return Check(false, "window for lifecycle");
    constexpr uint32 frameCounts[] = {1, 2, 3, 4, 8};
    const std::array<RenderVertex, 3> triangle{{{{-1, -1, 0}, {1, 0, 0, 1}, {0, 0}},
                                                {{3, -1, 0}, {1, 0, 0, 1}, {0, 0}},
                                                {{-1, 3, 0}, {1, 0, 0, 1}, {0, 0}}}};
    for (int round = 0; round < 5; ++round) {
        GpuDevice gpu;
        GpuDeviceConfig config;
        config.framesInFlight = frameCounts[round];
        Check(gpu.Initialize(window, config), "re-initialize on the same window");
        Check(gpu.GetInfo().framesInFlight == config.framesInFlight,
              "requested frame count retained");
        Check(RestoreForCapture(window), "lifecycle window restored for capture");
        GpuFrameCapture capture;
        const float shade = round / 4.f;
        Check(CaptureClear(window, gpu, shade, shade, shade, capture), "capture after re-init");
        const int expected = static_cast<int>(shade * 255.f + .5f);
        Check(CentreIs(capture, expected, expected, expected), "colour after re-init");
        for (uint32 slot = 0; slot < config.framesInFlight; ++slot)
            Check(gpu.RequestFrameCapture() && Begin(window, gpu) && gpu.Clear(0, 0, 0, 1) &&
                      gpu.DrawTriangles(triangle, Mat4{}) && gpu.EndFrame() &&
                      gpu.ReadFrameCapture(capture) && CentreIs(capture, 255, 0, 0),
                  "draw constants work on every frame slot");
        if (round % 2)
            gpu.Shutdown(); // explicit; even rounds rely on the destructor
    }
    GpuDevice same;
    Check(same.Initialize(window), "start again");
    same.Shutdown();
    Check(same.Initialize(window), "same GpuDevice starts after Shutdown");
    Check(RestoreForCapture(window), "restarted window restored for capture");
    GpuFrameCapture capture;
    Check(CaptureClear(window, same, 0, 0, 1, capture) && CentreIs(capture, 0, 0, 255),
          "renders after restart");
    GpuBuffer survivingBuffer;
    Check(survivingBuffer.Initialize(&same, BufferType::Vertex, 64),
          "buffer alive before closing rendering surface");
    Check(Begin(window, same) && same.Clear(0, 0, 0, 1), "window closes with a frame open");
    const auto skipped = same.GetCounters().framesSkipped;
    window.Shutdown();
    Check(!same.IsReady() && same.GetStatus() == GpuStatus::SurfaceLost &&
              same.GetLastFailure() == GpuStatus::SurfaceLost && !same.GetLastError().empty(),
          "window shutdown releases the native rendering surface and reports surface loss");
    Check(same.BeginFrame() == GpuDevice::FrameStart::Error && !same.EndFrame() &&
              same.GetCounters().framesSkipped == skipped && !survivingBuffer.IsValid(),
          "closed surfaces reject frames and invalidate resources");
    Check(window.Initialize(props), "window can be reopened after losing its old surface");
    Check(same.BeginFrame() == GpuDevice::FrameStart::Error,
          "reopening a window never reconnects its old swap chain");
    same.Shutdown();
    Check(same.Initialize(window) && CaptureClear(window, same, 0, 1, 0, capture) &&
              CentreIs(capture, 0, 255, 0),
          "an explicitly restarted device renders on the new window surface");
    same.Shutdown();
    window.Shutdown();

    GpuDevice outlivesWindow;
    {
        Window temporary;
        Check(temporary.Initialize(props) && outlivesWindow.Initialize(temporary),
              "temporary rendering window initializes");
    }
    Check(!outlivesWindow.IsReady() && outlivesWindow.GetStatus() == GpuStatus::SurfaceLost &&
              outlivesWindow.BeginFrame() == GpuDevice::FrameStart::Error,
          "device reports surface loss after the window object is destroyed");
}

void ManyFrames() {
    std::printf("[frames]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    config.framesInFlight = 3;
    Session s(256, 256, config);
    if (!s.ok)
        return Check(false, "session for frames");
    int mismatches = 0;
    for (int frame = 0; frame < 120; ++frame) {
        const int r = (frame * 37) % 256, g = (frame * 91) % 256, b = (frame * 13) % 256;
        GpuFrameCapture capture;
        if (!CaptureClear(s.window, s.gpu, r / 255.f, g / 255.f, b / 255.f, capture)) {
            ++mismatches;
            continue;
        }
        const uint8* p = &capture.rgba[(128 * 256 + 128) * 4];
        if (!Near(p[0], r) || !Near(p[1], g) || !Near(p[2], b))
            ++mismatches;
    }
    std::printf("  120 captured frames, %d mismatches\n", mismatches);
    Check(mismatches == 0, "every captured frame has its own colour");
    // Uncaptured frames at full speed.
    const uint64 before = s.gpu.GetCounters().framesSubmitted;
    const uint64 start = SDL_GetTicks();
    for (int frame = 0; frame < 500; ++frame)
        if (!(Begin(s.window, s.gpu) && s.gpu.Clear(frame % 2, 0, 0, 1) && s.gpu.EndFrame()))
            break;
    const uint64 elapsed = SDL_GetTicks() - start;
    const uint64 frames = s.gpu.GetCounters().framesSubmitted - before;
    std::printf("  %llu frames in %llu ms\n", static_cast<unsigned long long>(frames),
                static_cast<unsigned long long>(elapsed));
    Check(frames == 500, "500 frames without vsync");
    Check(s.gpu.SetVSync(true) && s.gpu.WaitIdle(), "vsync back on, idle");
    Check(s.gpu.GetStatus() == GpuStatus::Ready, "still ready");
}

std::vector<RenderVertex> Quad(float x0, float y0, float x1, float y1, float z, Vec4 color,
                               float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    auto v = [&](float x, float y, float u, float t) {
        RenderVertex vertex{};
        vertex.position = Vec3(x, y, z);
        vertex.color = color;
        vertex.uv = Vec2(u, t);
        return vertex;
    };
    return {v(x0, y0, u0, v0), v(x1, y0, u1, v0), v(x1, y1, u1, v1),
            v(x0, y0, u0, v0), v(x1, y1, u1, v1), v(x0, y1, u0, v1)};
}

const uint8* Pixel(const GpuFrameCapture& c, uint32 x, uint32 y) {
    return &c.rgba[(static_cast<size_t>(y) * c.width + x) * 4];
}

bool PixelIs(const GpuFrameCapture& c, uint32 x, uint32 y, int r, int g, int b, const char* what,
             int tolerance = 1) {
    if (c.rgba.empty() || x >= c.width || y >= c.height)
        return false;
    const uint8* p = Pixel(c, x, y);
    auto close = [&](int a, int e) { return a - e <= tolerance && e - a <= tolerance; };
    const bool ok = close(p[0], r) && close(p[1], g) && close(p[2], b);
    if (!ok)
        std::printf("  %s: pixel(%u,%u)=(%u,%u,%u,%u) expected (%d,%d,%d)\n", what, x, y, p[0],
                    p[1], p[2], p[3], r, g, b);
    return ok;
}

template <class F> bool CaptureFrame(Session& s, GpuFrameCapture& out, F&& draw) {
    if (!s.gpu.RequestFrameCapture() || !Begin(s.window, s.gpu))
        return false;
    const bool drawn = draw();
    return s.gpu.EndFrame() && drawn && s.gpu.ReadFrameCapture(out);
}

void Drawing() {
    std::printf("[drawing]\n");
    // A decorated Wayland window may be resized to the compositor's minimum title-bar
    // width after creation. Pixel-exact texture tests use an unmapped readback window.
    GpuDeviceConfig config;
    config.maxFrameVertexBytes = 64ull << 10;
    Session s(64, 64, config, true);
    if (!s.ok)
        return Check(false, "session for drawing");
    const Mat4 pixels = Mat4::Orthographic(0, 64, 64, 0, -1, 1);
    GpuFrameCapture c;

    Check(CaptureFrame(s, c,
                       [&] {
                           auto left = Quad(0, 0, 32, 64, 0, Vec4(1, 0, 0, 1));
                           auto top = Quad(32, 0, 64, 32, 0, Vec4(0, 1, 0, 1));
                           return s.gpu.Clear(0, 0, 0, 1) && s.gpu.DrawTriangles(left, pixels) &&
                                  s.gpu.DrawTriangles(top, pixels);
                       }),
          "solid quads drawn");
    Check(PixelIs(c, 16, 32, 255, 0, 0, "left half red"), "left half red");
    Check(PixelIs(c, 48, 16, 0, 255, 0, "top-right green (y points down)"), "top-right green");
    Check(PixelIs(c, 48, 48, 0, 0, 0, "bottom-right untouched"), "bottom-right black");
    Check(PixelIs(c, 31, 10, 255, 0, 0, "edge pixel 31 inside") &&
              PixelIs(c, 32, 50, 0, 0, 0, "edge pixel 32 outside"),
          "quad edges are pixel exact");
    const auto stats = s.gpu.GetFrameStats();
    Check(stats.drawCalls == 2 && stats.vertexBytes == 12 * sizeof(RenderVertex),
          "frame stats count draws");

    const auto budgetQuad = Quad(0, 0, 1, 1, 0, {1, 1, 1, 1});
    std::vector<RenderVertex> batch;
    for (int i = 0; i < 150; ++i)
        batch.insert(batch.end(), budgetQuad.begin(), budgetQuad.end());
    Check(Begin(s.window, s.gpu), "frame for the cumulative vertex limit");
    Check(s.gpu.DrawTriangles(batch, pixels), "first batch fits the vertex budget");
    Check(!s.gpu.DrawTriangles(batch, pixels) &&
              s.gpu.GetLastError().find("maxFrameVertexBytes") != std::string::npos,
          "a second batch cannot exceed the cumulative vertex budget");
    Check(s.gpu.DrawTriangles(budgetQuad, pixels) && s.gpu.EndFrame() &&
              s.gpu.GetFrameStats().vertexBytes ==
                  (batch.size() + budgetQuad.size()) * sizeof(RenderVertex),
          "a rejected batch preserves the remaining budget and frame submission");
    Check(Begin(s.window, s.gpu) && s.gpu.DrawTriangles(batch, pixels) && s.gpu.EndFrame(),
          "a new frame receives a fresh vertex budget");

    Check(CaptureFrame(s, c,
                       [&] {
                           auto half = Quad(0, 0, 64, 64, 0, Vec4(1, 0, 0, .5f));
                           return s.gpu.Clear(0, 0, 1, 1) && s.gpu.DrawTriangles(half, pixels);
                       }),
          "blended quad drawn");
    Check(PixelIs(c, 32, 32, 128, 0, 128, "50% red over blue", 2), "straight alpha blending");

    const uint8 checker[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    GpuTexture nearest;
    Check(nearest.Initialize(&s.gpu, 2, 2, checker, TextureFilter::Nearest),
          "nearest texture created");
    Check(CaptureFrame(s, c,
                       [&] {
                           auto quad = Quad(0, 0, 64, 64, 0, Vec4(1, 1, 1, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(quad, pixels, &nearest);
                       }),
          "textured quad drawn");
    Check(PixelIs(c, 8, 8, 255, 0, 0, "texel (0,0) top-left") &&
              PixelIs(c, 56, 8, 0, 255, 0, "texel (1,0)") &&
              PixelIs(c, 8, 56, 0, 0, 255, "texel (0,1)") &&
              PixelIs(c, 56, 56, 255, 255, 255, "texel (1,1)"),
          "nearest texture lands in the right quadrants");
    Check(PixelIs(c, 31, 31, 255, 0, 0, "no filtering at the border"),
          "nearest filter is not blended");

    GpuTexture linear;
    Check(linear.Initialize(&s.gpu, 2, 2, checker, TextureFilter::Linear),
          "linear texture created");
    Check(CaptureFrame(s, c,
                       [&] {
                           auto quad = Quad(0, 0, 64, 64, 0, Vec4(1, 1, 1, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(quad, pixels, &linear);
                       }),
          "linear textured quad drawn");
    const uint8* mid = Pixel(c, 32, 32);
    std::printf("  linear centre=(%u,%u,%u)\n", mid[0], mid[1], mid[2]);
    Check(mid[0] > 60 && mid[1] > 60 && mid[2] > 60 && mid[0] < 200, "linear filter blends texels");

    const uint8 grey[4] = {128, 128, 128, 255};
    GpuTexture srgb;
    Check(srgb.Initialize(&s.gpu, 1, 1, grey, TextureFilter::Nearest, TextureColorSpace::Srgb),
          "sRGB texture");
    Check(CaptureFrame(s, c,
                       [&] {
                           auto quad = Quad(0, 0, 64, 64, 0, Vec4(1, 1, 1, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(quad, pixels, &srgb);
                       }),
          "sRGB quad drawn");
    Check(PixelIs(c, 32, 32, 55, 55, 55, "sRGB 128 decodes to linear 0.216", 1), "sRGB decoding");

    // Thin textures (1xN, Nx1): left/right and top/bottom halves.
    const uint8 column[8] = {255, 0, 0, 255, 0, 0, 255, 255}; // 1x2: red over blue
    const uint8 row[8] = {0, 255, 0, 255, 255, 255, 0, 255};  // 2x1: green | yellow
    GpuTexture tall, wide;
    Check(tall.Initialize(&s.gpu, 1, 2, column, TextureFilter::Nearest) &&
              wide.Initialize(&s.gpu, 2, 1, row, TextureFilter::Nearest),
          "1x2 and 2x1 textures");
    Check(CaptureFrame(s, c,
                       [&] {
                           auto left = Quad(0, 0, 32, 64, 0, Vec4(1, 1, 1, 1));
                           auto right = Quad(32, 0, 64, 64, 0, Vec4(1, 1, 1, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(left, pixels, &tall) &&
                                  s.gpu.DrawTriangles(right, pixels, &wide);
                       }),
          "thin textures drawn");
    Check(c.width == 64 && c.height == 64, "pixel test retains its authored framebuffer size");
    Check(PixelIs(c, 16, 8, 255, 0, 0, "1x2 top") && PixelIs(c, 16, 56, 0, 0, 255, "1x2 bottom") &&
              PixelIs(c, 36, 32, 0, 255, 0, "2x1 left") &&
              PixelIs(c, 60, 32, 255, 255, 0, "2x1 right"),
          "thin textures sample correctly");

    Check(CaptureFrame(s, c,
                       [&] {
                           const auto quad = Quad(0, 0, 64, 64, 0, Vec4(1, 1, 1, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(quad, pixels, &wide);
                       }),
          "thin texture is reusable in a subsequent frame");
    Check(PixelIs(c, 8, 32, 0, 255, 0, "reused 2x1 left") &&
              PixelIs(c, 56, 32, 255, 255, 0, "reused 2x1 right"),
          "thin texture retains its own texels across frame slots");

    // Depth: z=+0.5 -> depth 0.25 (near), z=-0.5 -> 0.75 (far); LEQUAL keeps the nearest.
    Check(CaptureFrame(s, c,
                       [&] {
                           auto nearQuad = Quad(0, 0, 64, 64, .5f, Vec4(1, 0, 0, 1));
                           auto farQuad = Quad(0, 0, 64, 64, -.5f, Vec4(0, 0, 1, 1));
                           auto nearest2 = Quad(16, 16, 48, 48, .9f, Vec4(0, 1, 0, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(nearQuad, pixels, nullptr, true) &&
                                  s.gpu.DrawTriangles(farQuad, pixels, nullptr, true) &&
                                  s.gpu.DrawTriangles(nearest2, pixels, nullptr, true);
                       }),
          "depth-tested quads drawn");
    Check(PixelIs(c, 4, 4, 255, 0, 0, "near quad hides far quad"), "depth test rejects farther");
    Check(PixelIs(c, 32, 32, 0, 255, 0, "nearer quad wins"), "depth test accepts nearer");
    Check(CaptureFrame(s, c,
                       [&] {
                           auto farQuad = Quad(0, 0, 64, 64, -.5f, Vec4(0, 0, 1, 1));
                           return s.gpu.Clear(0, 0, 0, 1) &&
                                  s.gpu.DrawTriangles(farQuad, pixels, nullptr, true);
                       }),
          "depth cleared by Clear");
    Check(PixelIs(c, 32, 32, 0, 0, 255, "depth buffer cleared each frame"), "clear resets depth");

    // Misuse around drawing.
    auto quad = Quad(0, 0, 8, 8, 0, Vec4(1, 1, 1, 1));
    Check(!s.gpu.DrawTriangles(quad, pixels), "draw outside a frame rejected");
    Check(Begin(s.window, s.gpu), "frame for misuse");
    std::vector<RenderVertex> two(quad.begin(), quad.begin() + 2);
    Check(!s.gpu.DrawTriangles(two, pixels), "non-triangle vertex count rejected");
    Check(!s.gpu.DrawTriangles({}, pixels), "empty draw rejected");
    GpuTexture doomed;
    Check(doomed.Initialize(&s.gpu, 1, 1, grey), "texture to destroy");
    GpuTexture* doomedPtr = &doomed;
    doomed.Release();
    Check(!s.gpu.DrawTriangles(quad, pixels, doomedPtr), "draw with released texture rejected");
    Check(s.gpu.Clear(0, 0, 0, 1) && s.gpu.DrawTriangles(quad, pixels) && s.gpu.EndFrame(),
          "frame completes after rejected draws");
    Check(s.gpu.GetStatus() == GpuStatus::Ready, "device still ready");
}

void Volume() {
    std::printf("[volume]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    Session s(128, 128, config);
    if (!s.ok)
        return Check(false, "session for volume");
    const Mat4 pixels = Mat4::Orthographic(0, 128, 128, 0, -1, 1);

    // 5000 draws in one frame, each a 1x1 pixel quad.
    GpuFrameCapture c;
    Check(CaptureFrame(s, c,
                       [&] {
                           bool ok = s.gpu.Clear(0, 0, 0, 1);
                           for (int i = 0; i < 5000 && ok; ++i) {
                               const float x = static_cast<float>(i % 100),
                                           y = static_cast<float>(i / 100);
                               auto q = Quad(x, y, x + 1, y + 1, 0, Vec4(1, 1, 0, 1));
                               ok = s.gpu.DrawTriangles(q, pixels);
                           }
                           return ok;
                       }),
          "5000 draws in one frame");
    Check(s.gpu.GetFrameStats().drawCalls == 5000, "draw count");
    Check(PixelIs(c, 99, 49, 255, 255, 0, "last of 5000 draws") &&
              PixelIs(c, 100, 10, 0, 0, 0, "outside"),
          "every small draw landed");

    // One draw of 300k vertices (~14 MB): the vertex ring must grow.
    std::vector<RenderVertex> big;
    big.reserve(300000);
    while (big.size() + 6 <= 300000) {
        auto q = Quad(0, 0, 128, 128, 0, Vec4(0, 1, 1, 1));
        big.insert(big.end(), q.begin(), q.end());
    }
    Check(CaptureFrame(s, c,
                       [&] { return s.gpu.Clear(0, 0, 0, 1) && s.gpu.DrawTriangles(big, pixels); }),
          "300k-vertex draw");
    Check(PixelIs(c, 64, 64, 0, 255, 255, "big draw"), "big draw visible");
    // Mixed: grow the ring in the middle of a frame; earlier draws must survive.
    Check(CaptureFrame(s, c,
                       [&] {
                           auto a = Quad(0, 0, 64, 128, 0, Vec4(1, 0, 0, 1));
                           bool ok = s.gpu.Clear(0, 0, 0, 1) && s.gpu.DrawTriangles(a, pixels);
                           ok = ok && s.gpu.DrawTriangles(big, pixels);
                           ok = ok && s.gpu.DrawTriangles(big, pixels);
                           auto b = Quad(0, 0, 64, 128, 0, Vec4(1, 0, 1, 1));
                           return ok && s.gpu.DrawTriangles(b, pixels);
                       }),
          "ring growth inside a frame");
    Check(PixelIs(c, 32, 64, 255, 0, 255, "draw after growth") &&
              PixelIs(c, 96, 64, 0, 255, 255, "big draws"),
          "draws before and after growth");

    // Texture churn: 300 textures created, drawn and destroyed across frames.
    int failures = 0;
    for (int round = 0; round < 30; ++round) {
        std::vector<GpuTexture> batch(10);
        for (int i = 0; i < 10; ++i) {
            const uint8 px[4] = {static_cast<uint8>(round * 8), static_cast<uint8>(i * 25), 0, 255};
            if (!batch[i].Initialize(&s.gpu, 1, 1, px, TextureFilter::Nearest))
                ++failures;
        }
        if (!Begin(s.window, s.gpu)) {
            ++failures;
            continue;
        }
        bool ok = s.gpu.Clear(0, 0, 0, 1);
        for (int i = 0; i < 10 && ok; ++i) {
            auto q = Quad(i * 12.f, 0, i * 12.f + 12, 12, 0, Vec4(1, 1, 1, 1));
            ok = s.gpu.DrawTriangles(q, pixels, &batch[i]);
        }
        if (!ok || !s.gpu.EndFrame())
            ++failures;
        // batch destroyed here while the frame may still be in flight
    }
    Check(failures == 0, "texture churn without failures");
    for (int i = 0; i < 4; ++i)
        Check(Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1) && s.gpu.EndFrame(),
              "drain frames");
    Check(s.gpu.WaitIdle(), "idle");
    const auto memory = s.gpu.GetMemoryStats();
    std::printf("  live textures %u, pending %u\n", memory.liveTextures,
                memory.pendingDestructions);
    Check(memory.liveTextures == 0 && memory.pendingDestructions == 0, "all textures released");

    // Texture outliving its device.
    GpuTexture orphan;
    Check(orphan.Initialize(&s.gpu, 4, 4, nullptr), "orphan texture");
    s.gpu.Shutdown();
    Check(!orphan.IsValid(), "texture invalid after device shutdown");
    orphan.Release();
    Check(true, "orphan release is safe");
}

// Small -> large -> fullscreen, several rounds. Each size draws a pattern (four coloured
// corner quads + a centre quad) and checks it is placed correctly at that resolution.
bool PatternOk(Session& s, uint32 w, uint32 h, const char* label) {
    const float fw = static_cast<float>(w), fh = static_cast<float>(h);
    const Mat4 pixels = Mat4::Orthographic(0, fw, fh, 0, -1, 1);
    const float qw = fw / 4, qh = fh / 4;
    GpuFrameCapture c;
    const bool drawn = CaptureFrame(s, c, [&] {
        auto tl = Quad(0, 0, qw, qh, 0, Vec4(1, 0, 0, 1));
        auto tr = Quad(fw - qw, 0, fw, qh, 0, Vec4(0, 1, 0, 1));
        auto bl = Quad(0, fh - qh, qw, fh, 0, Vec4(0, 0, 1, 1));
        auto br = Quad(fw - qw, fh - qh, fw, fh, 0, Vec4(1, 1, 0, 1));
        auto mid = Quad(fw / 2 - qw / 2, fh / 2 - qh / 2, fw / 2 + qw / 2, fh / 2 + qh / 2, .5f,
                        Vec4(1, 1, 1, 1));
        return s.gpu.Clear(.1f, .1f, .1f, 1) && s.gpu.DrawTriangles(tl, pixels) &&
               s.gpu.DrawTriangles(tr, pixels) && s.gpu.DrawTriangles(bl, pixels) &&
               s.gpu.DrawTriangles(br, pixels) && s.gpu.DrawTriangles(mid, pixels, nullptr, true);
    });
    if (!drawn || c.width != w || c.height != h) {
        std::printf(
            "  %s %ux%u: drawn=%d capture %ux%u status=%s minimized=%d fullscreen=%d error=%s\n",
            label, w, h, drawn ? 1 : 0, c.width, c.height, ToString(s.gpu.GetStatus()),
            s.window.IsMinimized() ? 1 : 0, s.window.IsFullscreen() ? 1 : 0,
            s.gpu.GetLastError().c_str());
        return false;
    }
    const uint32 x0 = 0, y0 = 0, x1 = w - 1, y1 = h - 1;
    const bool ok = PixelIs(c, x0, y0, 255, 0, 0, "top-left") &&
                    PixelIs(c, x1, y0, 0, 255, 0, "top-right") &&
                    PixelIs(c, x0, y1, 0, 0, 255, "bottom-left") &&
                    PixelIs(c, x1, y1, 255, 255, 0, "bottom-right") &&
                    PixelIs(c, w / 2, h / 2, 255, 255, 255, "centre") &&
                    (w < 8 || h < 8 || PixelIs(c, w / 2, h / 8, 25, 25, 25, "background"));
    std::printf("  %-10s %5ux%-5u %s\n", label, w, h, ok ? "ok" : "WRONG");
    return ok;
}

void Resolutions() {
    std::printf("[resolutions]\n");
    Session s(64, 64);
    if (!s.ok)
        return Check(false, "session for resolutions");
    const uint32 sizes[][2] = {{8, 8},     {64, 48},    {160, 120},  {320, 240},  {640, 480},
                               {800, 600}, {1024, 768}, {1280, 720}, {1600, 900}, {1920, 1080}};
    for (int round = 1; round <= 3; ++round) {
        std::printf("  round %d\n", round);
        for (const auto& size : sizes) {
            s.window.SetSize(size[0], size[1]);
            for (int i = 0; i < 5; ++i) {
                s.window.PollEvents();
                SDL_Delay(5);
            }
            uint32 fw = 0, fh = 0;
            s.window.GetFramebufferSize(fw, fh);
            if (fw != size[0] || fh != size[1])
                std::printf("  (window manager gave %ux%u for %ux%u)\n", fw, fh, size[0], size[1]);
            Check(PatternOk(s, fw, fh, "windowed"), "pattern at window size");
        }
        s.window.SetFullscreen(true);
        bool full = false;
        for (int i = 0; i < 200 && !full; ++i) {
            s.window.PollEvents();
            SDL_Delay(10);
            full = s.window.IsFullscreen();
        }
        for (int i = 0; i < 20; ++i) {
            s.window.PollEvents();
            SDL_Delay(10);
        }
        uint32 fw = 0, fh = 0;
        s.window.GetFramebufferSize(fw, fh);
        Check(full, "fullscreen entered");
        Check(PatternOk(s, fw, fh, "fullscreen"), "pattern in fullscreen");
        // A few frames in fullscreen without capture, then back to a window.
        for (int i = 0; i < 30; ++i)
            Check(Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1) && s.gpu.EndFrame(),
                  "fullscreen frame");
        s.window.SetFullscreen(false);
        for (int i = 0; i < 50; ++i) {
            s.window.PollEvents();
            SDL_Delay(10);
        }
        s.window.SetSize(320, 240);
        for (int i = 0; i < 10; ++i) {
            s.window.PollEvents();
            SDL_Delay(5);
        }
        s.window.GetFramebufferSize(fw, fh);
        Check(!s.window.IsFullscreen() && PatternOk(s, fw, fh, "windowed"),
              "pattern after leaving fullscreen");
    }
    Check(s.gpu.GetStatus() == GpuStatus::Ready, "device ready after resolution changes");
}

// The window "breathes": its size follows two sine waves every frame while the clear colour
// cycles through the hue wheel. Every 10th frame is captured and must have the current window
// size and the current colour; a quad pattern is drawn on top and checked at its corners.
long long g_WobbleFrames = 600;

void HueToRgb(float h, float& r, float& g, float& b) {
    const float x = h * 6.0f;
    r = std::clamp(std::fabs(x - 3.0f) - 1.0f, 0.0f, 1.0f);
    g = std::clamp(2.0f - std::fabs(x - 2.0f), 0.0f, 1.0f);
    b = std::clamp(2.0f - std::fabs(x - 4.0f), 0.0f, 1.0f);
}

void Wobble() {
    std::printf("[wobble] %lld frames\n", g_WobbleFrames);
    GpuDeviceConfig config;
    config.vsync = false;
    Session s(400, 300, config);
    if (!s.ok)
        return Check(false, "session for wobble");
    long long captured = 0, wrongSize = 0, wrongColour = 0, wrongPattern = 0, failedFrames = 0;
    uint32 minW = 1u << 30, maxW = 0, minH = 1u << 30, maxH = 0;
    const uint64 start = SDL_GetTicks();
    for (long long frame = 0; frame < g_WobbleFrames; ++frame) {
        if (frame > 0 && frame % 10000 == 0)
            std::printf("  progress %lld/%lld frames, %llu s: failed %lld, size %lld, colour %lld, "
                        "quad %lld\n",
                        frame, g_WobbleFrames,
                        static_cast<unsigned long long>((SDL_GetTicks() - start) / 1000),
                        failedFrames, wrongSize, wrongColour, wrongPattern);
        const float t = static_cast<float>(std::fmod(frame * 0.05, 2 * 3.14159265358979 * 100.0));
        const uint32 w = static_cast<uint32>(700 + 500 * std::sin(t));
        const uint32 h = static_cast<uint32>(500 + 350 * std::sin(t * 1.37f + 1.0f));
        s.window.SetSize(w, h);
        s.window.PollEvents();
        uint32 fw = 0, fh = 0;
        s.window.GetFramebufferSize(fw, fh);
        minW = std::min(minW, fw), maxW = std::max(maxW, fw), minH = std::min(minH, fh),
        maxH = std::max(maxH, fh);
        float r, g, b;
        HueToRgb(static_cast<float>(std::fmod(frame * 0.013, 1.0)), r, g, b);
        const bool capture = frame % 10 == 0;
        if (capture && !s.gpu.RequestFrameCapture()) {
            ++failedFrames;
            continue;
        }
        if (!Begin(s.window, s.gpu)) {
            ++failedFrames;
            continue;
        }
        const Mat4 pixels =
            Mat4::Orthographic(0, static_cast<float>(fw), static_cast<float>(fh), 0, -1, 1);
        auto corner = Quad(0, 0, 16, 16, 0, Vec4(1 - r, 1 - g, 1 - b, 1));
        bool ok =
            s.gpu.Clear(r, g, b, 1) && s.gpu.DrawTriangles(corner, pixels) && s.gpu.EndFrame();
        if (!ok) {
            ++failedFrames;
            continue;
        }
        if (!capture)
            continue;
        GpuFrameCapture c;
        if (!s.gpu.ReadFrameCapture(c)) {
            ++failedFrames;
            continue;
        }
        ++captured;
        if (c.width != fw || c.height != fh) {
            if (wrongSize++ < 5)
                std::printf("  frame %lld: capture %ux%u, window %ux%u\n", frame, c.width, c.height,
                            fw, fh);
            continue;
        }
        const int er = static_cast<int>(r * 255 + .5f), eg = static_cast<int>(g * 255 + .5f),
                  eb = static_cast<int>(b * 255 + .5f);
        const uint8* centre = Pixel(c, fw / 2, fh / 2);
        if (std::abs(centre[0] - er) > 1 || std::abs(centre[1] - eg) > 1 ||
            std::abs(centre[2] - eb) > 1) {
            if (wrongColour++ < 5)
                std::printf("  frame %lld: centre (%u,%u,%u) expected (%d,%d,%d)\n", frame,
                            centre[0], centre[1], centre[2], er, eg, eb);
        }
        const uint8* tl = Pixel(c, 4, 4);
        if (std::abs(tl[0] - (255 - er)) > 1 || std::abs(tl[1] - (255 - eg)) > 1 ||
            std::abs(tl[2] - (255 - eb)) > 1) {
            if (wrongPattern++ < 5)
                std::printf("  frame %lld: corner quad (%u,%u,%u) expected (%d,%d,%d)\n", frame,
                            tl[0], tl[1], tl[2], 255 - er, 255 - eg, 255 - eb);
        }
    }
    const uint64 elapsed = SDL_GetTicks() - start;
    std::printf("  sizes %u..%u x %u..%u, %lld captures, %llu ms\n", minW, maxW, minH, maxH,
                captured, static_cast<unsigned long long>(elapsed));
    std::printf(
        "  failed frames %lld, wrong size %lld, wrong colour %lld, wrong corner quad %lld\n",
        failedFrames, wrongSize, wrongColour, wrongPattern);
    Check(failedFrames == 0, "wobble: every frame succeeds");
    Check(wrongSize == 0, "wobble: swap chain always matches the window");
    Check(wrongColour == 0, "wobble: clear colour follows every frame");
    Check(wrongPattern == 0, "wobble: drawn quad follows every frame");
    Check(s.gpu.GetStatus() == GpuStatus::Ready, "wobble: device ready");
}

// Lit meshes through the backend's buffers + DrawIndexed, checked against the shading formulas
// of the Vulkan backend's Basic shaders (Lambert + 0.15 ambient, point-light falloff, PBR path).
struct BackendSession {
    Window window;
    Ref<Rhi::RenderBackend> backend;
    bool ok = false;

    BackendSession(uint32 width, uint32 height) {
        WindowProps props;
        props.width = width;
        props.height = height;
        props.title = "LuxEngine Forge mesh verification";
        props.vulkan = false;
        if (!window.Initialize(props))
            return;
        GpuDeviceConfig config;
        backend = Rhi::CreateRenderBackend(config);
        ok = backend && backend->Initialize(window, config);
        if (!ok && backend)
            std::printf("  init failed: %s\n", backend->GetLastError().c_str());
    }

    ~BackendSession() {
        if (backend)
            backend->Shutdown();
        window.Shutdown();
    }

    bool Begin() {
        for (int i = 0; i < 200; ++i) {
            window.PollEvents();
            const auto start = backend->BeginFrame();
            if (start == GpuDevice::FrameStart::Ready)
                return true;
            if (start == GpuDevice::FrameStart::Error)
                return false;
            SDL_Delay(5);
        }
        return false;
    }

    template <class F> bool Capture(GpuFrameCapture& out, F&& draw) {
        if (!backend->RequestFrameCapture() || !Begin())
            return false;
        const bool drawn = draw();
        return backend->EndFrame() && drawn && backend->ReadFrameCapture(out);
    }
};

struct CubeMesh {
    uint64 vertices = 0, indices = 0;
    Rhi::RenderBackend* owner = nullptr;

    ~CubeMesh() {
        if (owner) {
            owner->DestroyBuffer(vertices);
            owner->DestroyBuffer(indices);
        }
    }

    static void Build(Vec4 colour, std::vector<RenderVertex>& v, std::vector<uint32>& idx) {
        const Vec3 n[6] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (int f = 0; f < 6; ++f) {
            const Vec3 normal = n[f];
            const Vec3 a = std::fabs(normal.y) > .5f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
            const Vec3 bb = Vec3(normal.y * a.z - normal.z * a.y, normal.z * a.x - normal.x * a.z,
                                 normal.x * a.y - normal.y * a.x);
            const uint32 base = static_cast<uint32>(v.size());
            const float signs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            for (const auto& sg : signs) {
                RenderVertex vertex{};
                vertex.position = Vec3(normal.x + a.x * sg[0] + bb.x * sg[1],
                                       normal.y + a.y * sg[0] + bb.y * sg[1],
                                       normal.z + a.z * sg[0] + bb.z * sg[1]);
                vertex.color = colour;
                vertex.uv = Vec2((sg[0] + 1) / 2, (sg[1] + 1) / 2);
                vertex.normal = normal;
                v.push_back(vertex);
            }
            for (uint32 k : {0u, 1u, 2u, 0u, 2u, 3u})
                idx.push_back(base + k);
        }
    }

    bool Create(Rhi::RenderBackend& b, Vec4 colour = Vec4(1, 1, 1, 1)) {
        owner = &b;
        std::vector<RenderVertex> v;
        std::vector<uint32> idx;
        Build(colour, v, idx);
        const uint32 vb = uint32(v.size() * sizeof(RenderVertex)),
                     ib = uint32(idx.size() * sizeof(uint32));
        return b.CreateBuffer(BufferType::Vertex, vb, vertices) &&
               b.UpdateBuffer(vertices, v.data(), vb, 0) &&
               b.CreateBuffer(BufferType::Index, ib, indices) &&
               b.UpdateBuffer(indices, idx.data(), ib, 0);
    }
};

int Unorm(float v) {
    return static_cast<int>(std::clamp(v, 0.f, 1.f) * 255.f + .5f);
}

void Meshes() {
    std::printf("[meshes]\n");
    BackendSession s(128, 128);
    if (!s.ok)
        return Check(false, "backend session for meshes");
    Rhi::RenderBackend& b = *s.backend;
    CubeMesh cube;
    Check(cube.Create(b), "cube buffers created and filled");
    const Mat4 vp = Mat4::Orthographic(-2, 2, -2, 2, -10, 10);
    const Mat4 identity;
    const Vec4 albedo(.5f, .25f, .125f, 1);
    GpuFrameCapture c;
    auto draw = [&](const GpuLighting* lighting, const Mat4& model, const Vec4& colour,
                    bool pbr = false, CubeMesh* mesh = nullptr) {
        CubeMesh& m = mesh ? *mesh : cube;
        return s.Capture(c, [&] {
            return b.Clear(0, 0, 0, 1) && (!lighting || b.SetLighting(*lighting)) &&
                   b.DrawIndexed(m.vertices, m.indices, 36, vp, model, colour, 0, 0.f, .5f, pbr);
        });
    };
    Check(draw(nullptr, identity, albedo), "mesh without lighting");
    Check(PixelIs(c, 64, 64, Unorm(.5f * .15f), Unorm(.25f * .15f), Unorm(.125f * .15f),
                  "ambient only"),
          "ambient term");
    Check(PixelIs(c, 2, 2, 0, 0, 0, "outside the cube"), "background outside the mesh");
    GpuLighting sun;
    sun.direction = Vec3(0, 0, -1);
    sun.color = Vec3(1, 1, 1);
    sun.intensity = 1;
    Check(draw(&sun, identity, albedo), "mesh with directional light");
    Check(PixelIs(c, 64, 64, Unorm(.5f * 1.15f), Unorm(.25f * 1.15f), Unorm(.125f * 1.15f),
                  "lambert front face"),
          "directional light (Lambert + ambient)");
    GpuLighting side = sun;
    side.direction = Vec3(-1, 0, 0);
    Check(draw(&side, identity, albedo) && PixelIs(c, 64, 64, Unorm(.5f * .15f), Unorm(.25f * .15f),
                                                   Unorm(.125f * .15f), "light parallel to face"),
          "light direction matters");
    GpuPointLight point;
    point.position = Vec3(0, 0, 3);
    point.radius = 10;
    point.color = Vec3(1, 1, 1);
    point.intensity = 1;
    GpuLighting lamp;
    lamp.direction = Vec3(0, 0, -1);
    lamp.intensity = 0;
    lamp.points = std::span<const GpuPointLight>(&point, 1);
    Check(draw(&lamp, identity, albedo) && PixelIs(c, 64, 64, Unorm(.5f * .79f), Unorm(.25f * .79f),
                                                   Unorm(.125f * .79f), "point light", 2),
          "point light falloff");
    Check(draw(&sun, Mat4::Translate(Vec3(1.5f, 0, 0)), albedo), "translated mesh");
    Check(PixelIs(c, 64, 64, 0, 0, 0, "old centre empty") &&
              PixelIs(c, 112, 64, Unorm(.5f * 1.15f), Unorm(.25f * 1.15f), Unorm(.125f * 1.15f),
                      "moved cube"),
          "model matrix applied");
    Check(draw(&sun, Mat4::Scale(Vec3(.5f, .5f, 3.f)), albedo) &&
              PixelIs(c, 64, 64, Unorm(.5f * 1.15f), Unorm(.25f * 1.15f), Unorm(.125f * 1.15f),
                      "scaled cube"),
          "non-uniform scale keeps lighting");
    GpuLighting pbrSun = sun;
    pbrSun.pbr = true;
    pbrSun.camera = Vec3(0, 0, 5);
    Check(draw(&pbrSun, identity, albedo, true), "PBR mesh");
    const uint8* p = Pixel(c, 64, 64);
    std::printf("  PBR centre=(%u,%u,%u)\n", p[0], p[1], p[2]);
    Check(p[0] > p[1] && p[1] > p[2] && p[0] > 60 && p[0] < 255, "PBR shading plausible");
    CubeMesh tinted;
    Check(tinted.Create(b, Vec4(.5f, 1, 1, 1)), "second mesh");
    Check(draw(&sun, identity, albedo, false, &tinted) &&
              PixelIs(c, 64, 64, Unorm(.25f * 1.15f), Unorm(.25f * 1.15f), Unorm(.125f * 1.15f),
                      "vertex colour"),
          "vertex colours multiply albedo");
    Check(s.Begin() && b.Clear(0, 0, 0, 1), "frame for misuse");
    Check(!b.DrawIndexed(cube.vertices, cube.indices, 37, vp, identity, albedo, 0, 0, .5f, false),
          "index count not /3 rejected");
    Check(!b.DrawIndexed(cube.vertices, cube.indices, 39, vp, identity, albedo, 0, 0, .5f, false),
          "index count beyond buffer rejected");
    Check(!b.DrawIndexed(cube.indices, cube.vertices, 36, vp, identity, albedo, 0, 0, .5f, false),
          "swapped buffers rejected");
    Check(!b.DrawIndexed(cube.vertices, 999999, 36, vp, identity, albedo, 0, 0, .5f, false),
          "unknown buffer rejected");
    int lightCalls = 0;
    while (lightCalls < 400 && b.SetLighting(sun))
        ++lightCalls;
    std::printf("  SetLighting calls accepted in one frame: %d\n", lightCalls);
    Check(lightCalls == 255, "SetLighting limit per frame");
    Check(b.EndFrame() && b.GetStatus() == GpuStatus::Ready, "device ready after mesh misuse");
    // Re-upload the same vertex buffer every frame while earlier frames may still read it.
    for (int round = 0; round < 20; ++round) {
        std::vector<RenderVertex> v;
        std::vector<uint32> idx;
        CubeMesh::Build(Vec4(round % 2 ? 1.f : .5f, 1, 1, 1), v, idx);
        Check(b.UpdateBuffer(tinted.vertices, v.data(), uint32(v.size() * sizeof(RenderVertex)), 0),
              "vertex buffer re-uploaded");
        Check(draw(&sun, identity, albedo, false, &tinted) &&
                  PixelIs(c, 64, 64, Unorm((round % 2 ? .5f : .25f) * 1.15f), Unorm(.25f * 1.15f),
                          Unorm(.125f * 1.15f), "re-uploaded mesh"),
              "updated buffer is drawn with the new data");
    }
    // Partial update at an offset (only the front face's 4 vertices get green).
    {
        std::vector<RenderVertex> v;
        std::vector<uint32> idx;
        CubeMesh::Build(Vec4(0, 1, 0, 1), v, idx);
        Check(b.UpdateBuffer(tinted.vertices, v.data(), 4 * sizeof(RenderVertex), 0),
              "partial update");
        Check(!b.UpdateBuffer(tinted.vertices, v.data(), 24 * sizeof(RenderVertex),
                              sizeof(RenderVertex)),
              "update past the end rejected");
        Check(draw(&sun, identity, albedo, false, &tinted) &&
                  PixelIs(c, 64, 64, 0, Unorm(.25f * 1.15f), 0, "front face green"),
              "partial buffer update");
    }
    std::vector<CubeMesh> many(50);
    int created = 0;
    for (auto& m : many)
        created += m.Create(b) ? 1 : 0;
    Check(created == 50, "50 meshes");
    for (int frame = 0; frame < 10; ++frame) {
        bool ok = s.Begin() && b.Clear(0, 0, 0, 1) && b.SetLighting(sun);
        for (int i = 0; i < 2000 && ok; ++i) {
            const auto& m = many[i % 50];
            ok = b.DrawIndexed(m.vertices, m.indices, 36, vp,
                               Mat4::Translate(Vec3((i % 40) * .1f - 2, (i / 40) * .08f - 2, 0)) *
                                   Mat4::Scale(Vec3(.05f, .05f, .05f)),
                               albedo, 0, 0, .5f, false);
        }
        Check(ok && b.EndFrame(), "2000 mesh draws per frame");
    }
    many.clear(); // destroyed while frames are in flight
    for (int i = 0; i < 4; ++i)
        s.Begin() && b.Clear(0, 0, 0, 1) && b.EndFrame();
    Check(b.WaitIdle() && b.GetStatus() == GpuStatus::Ready, "buffers released while in flight");
}

// Endurance: g_FramesPerSize frames at each window size (and fullscreen). Every frame clears
// to a cycling colour and draws a moving solid quad, a textured quad and a depth-tested quad;
// every g_CaptureEvery-th frame is read back and checked pixel by pixel at known points.
long long g_FramesPerSize = 1000000;
int g_CaptureEvery = 100;

void Endurance() {
    std::printf("[endurance] %lld frames per size, capture every %d\n", g_FramesPerSize,
                g_CaptureEvery);
    GpuDeviceConfig config;
    config.vsync = false;
    Session s(320, 240, config);
    if (!s.ok)
        return Check(false, "session for endurance");
    const uint8 checker[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    GpuTexture texture;
    Check(texture.Initialize(&s.gpu, 2, 2, checker, TextureFilter::Nearest), "endurance texture");

    struct Size {
        uint32 w, h;
        bool fullscreen;
    };

    const Size sizes[] = {{64, 64, false},    {320, 240, false},   {640, 480, false},
                          {1280, 720, false}, {1920, 1080, false}, {0, 0, true}};
    const uint64 total = SDL_GetTicks();
    for (const Size& size : sizes) {
        if (size.fullscreen) {
            s.window.SetFullscreen(true);
            for (int i = 0; i < 100 && !s.window.IsFullscreen(); ++i) {
                s.window.PollEvents();
                SDL_Delay(10);
            }
        } else {
            s.window.SetFullscreen(false);
            s.window.SetSize(size.w, size.h);
        }
        for (int i = 0; i < 30; ++i) {
            s.window.PollEvents();
            SDL_Delay(5);
        }
        uint32 fw = 0, fh = 0;
        s.window.GetFramebufferSize(fw, fh);
        const float W = static_cast<float>(fw), H = static_cast<float>(fh);
        const Mat4 pixels = Mat4::Orthographic(0, W, H, 0, -1, 1);
        const char* label = size.fullscreen ? "fullscreen" : "windowed";
        std::printf("  == %s %ux%u ==\n", label, fw, fh);
        long long failed = 0, captures = 0, wrong = 0;
        const uint64 start = SDL_GetTicks();
        for (long long frame = 0; frame < g_FramesPerSize; ++frame) {
            if (frame % 10000 == 0 && StopRequested())
                break;
            if (frame > 0 && frame % 50000 == 0) {
                const double secs = (SDL_GetTicks() - start) / 1000.0;
                std::printf("  %ux%u progress %lld/%lld, %.0f s, %.0f fps: failed %lld, wrong %lld "
                            "/ %lld captures\n",
                            fw, fh, frame, g_FramesPerSize, secs, frame / (secs > 0 ? secs : 1),
                            failed, wrong, captures);
            }
            s.window.PollEvents();
            float r, g, b;
            HueToRgb(static_cast<float>(std::fmod(frame * 0.0037, 1.0)), r, g, b);
            const bool capture = g_CaptureEvery > 0 && frame % g_CaptureEvery == 0;
            if (capture && !s.gpu.RequestFrameCapture()) {
                ++failed;
                continue;
            }
            const auto begun = s.gpu.BeginFrame();
            if (begun == GpuDevice::FrameStart::Skipped)
                continue;
            if (begun != GpuDevice::FrameStart::Ready) {
                if (failed++ < 5)
                    std::printf("  frame %lld: BeginFrame failed: %s\n", frame,
                                s.gpu.GetLastError().c_str());
                continue;
            }
            // Moving solid quad sweeps the top strip; textured quad bottom-left quarter; depth quad
            // centre.
            const float bw = std::max(4.f, W / 8),
                        sweep = static_cast<float>(frame % 64) / 64.f * (W - bw);
            auto moving =
                Quad(sweep, 0, sweep + bw, std::max(2.f, H / 8), 0, Vec4(1 - r, 1 - g, 1 - b, 1));
            auto textured = Quad(0, H / 2, W / 2, H, 0, Vec4(1, 1, 1, 1));
            auto depthQuad =
                Quad(W * .375f, H * .375f, W * .625f, H * .625f, .5f, Vec4(1, 1, 1, 1));
            const bool ok = s.gpu.Clear(r, g, b, 1) && s.gpu.DrawTriangles(moving, pixels) &&
                            s.gpu.DrawTriangles(textured, pixels, &texture) &&
                            s.gpu.DrawTriangles(depthQuad, pixels, nullptr, true) &&
                            s.gpu.EndFrame();
            if (!ok) {
                if (failed++ < 5)
                    std::printf("  frame %lld: frame failed: %s\n", frame,
                                s.gpu.GetLastError().c_str());
                continue;
            }
            if (!capture)
                continue;
            GpuFrameCapture c;
            if (!s.gpu.ReadFrameCapture(c)) {
                ++failed;
                continue;
            }
            ++captures;
            const int er = static_cast<int>(r * 255 + .5f), eg = static_cast<int>(g * 255 + .5f),
                      eb = static_cast<int>(b * 255 + .5f);
            auto near1 = [](int a, int e) { return a - e <= 1 && e - a <= 1; };
            auto at = [&](float x, float y, int R, int G, int B) {
                const uint32 px = std::min(fw - 1, static_cast<uint32>(x)),
                             py = std::min(fh - 1, static_cast<uint32>(y));
                const uint8* p = Pixel(c, px, py);
                return near1(p[0], R) && near1(p[1], G) && near1(p[2], B);
            };
            const bool good =
                c.width == fw && c.height == fh &&
                at(W * .875f, H * .375f, er, eg, eb) && // background
                at(sweep + bw / 2, std::max(1.f, H / 16), 255 - er, 255 - eg, 255 - eb) && // moving
                at(W / 8, H * 5 / 8, 255, 0, 0) &&
                at(W * 3 / 8, H * 7 / 8, 255, 255, 255) && // texture
                at(W / 2, H / 2, 255, 255, 255);           // depth quad
            if (!good && wrong++ < 5) {
                const uint8* bg = Pixel(c, std::min(fw - 1, static_cast<uint32>(W * .875f)),
                                        std::min(fh - 1, static_cast<uint32>(H * .375f)));
                std::printf(
                    "  frame %lld: capture %ux%u wrong (background %u,%u,%u expected %d,%d,%d)\n",
                    frame, c.width, c.height, bg[0], bg[1], bg[2], er, eg, eb);
            }
        }
        const double secs = (SDL_GetTicks() - start) / 1000.0;
        std::printf("  RESULT %s %ux%u: %lld frames in %.0f s (%.0f fps), %lld captures checked, "
                    "%lld wrong, %lld failed\n",
                    label, fw, fh, g_FramesPerSize, secs, g_FramesPerSize / (secs > 0 ? secs : 1),
                    captures, wrong, failed);
        Check(failed == 0, "endurance: no failed frames");
        Check(wrong == 0, "endurance: every checked capture correct");
        Check(s.gpu.GetStatus() == GpuStatus::Ready, "endurance: device ready");
    }
    s.window.SetFullscreen(false);
    std::printf("  endurance total %.1f min\n", (SDL_GetTicks() - total) / 60000.0);
}

// ---------------------------------------------------------------------------------------
// Stress scenes: realistic loads with geometric checks (positions / occlusion), not just colours.
long long g_SceneFrames = 3000;
long long g_TotalFrames = 0;
double g_RunSeconds = 0; // > 0: mix runs for this long instead of a frame count

// "HH:MM" (local time, today or tomorrow) -> seconds from now.
double SecondsUntil(const std::string& hhmm) {
    int hh = 0, mm = 0;
    if (std::sscanf(hhmm.c_str(), "%d:%d", &hh, &mm) != 2)
        return 0;
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    std::tm target = local;
    target.tm_hour = hh, target.tm_min = mm, target.tm_sec = 0;
    double diff = std::difftime(std::mktime(&target), now);
    if (diff < 0)
        diff += 24 * 3600;
    return diff;
}

int g_ParticleCount = 50000;
bool g_QuietScenes = false;

struct Rng {
    uint32 state;

    explicit Rng(uint32 seed) : state(seed ? seed : 1) {}

    uint32 Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float Unit() {
        return (Next() & 0xFFFFFF) / float(0x1000000);
    }
};

void PushQuad(std::vector<RenderVertex>& out, float cx, float cy, float half, float angle,
              Vec4 color, float u0 = 0, float v0 = 0, float u1 = 1, float v1 = 1) {
    const float c = std::cos(angle) * half, sn = std::sin(angle) * half;
    auto v = [&](float dx, float dy, float u, float t) {
        RenderVertex vertex{};
        vertex.position = Vec3(cx + dx * c - dy * sn, cy + dx * sn + dy * c, 0);
        vertex.color = color;
        vertex.uv = Vec2(u, t);
        out.push_back(vertex);
    };
    v(-1, -1, u0, v0), v(1, -1, u1, v0), v(1, 1, u1, v1);
    v(-1, -1, u0, v0), v(1, 1, u1, v1), v(-1, 1, u0, v1);
}

struct SceneStats {
    long long frames = 0, failed = 0, checks = 0, wrong = 0;
    double seconds = 0;
};

std::map<std::string, SceneStats> g_SceneTotals;

void Report(const char* name, const SceneStats& st) {
    if (g_QuietScenes) {
        auto& total = g_SceneTotals[name];
        total.frames += st.frames, total.failed += st.failed, total.checks += st.checks,
            total.wrong += st.wrong;
        total.seconds += st.seconds;
        if (st.failed || st.wrong)
            std::printf("  %s segment: %lld wrong, %lld failed\n", name, st.wrong, st.failed);
        Check(st.failed == 0 && st.wrong == 0, "stress scene segment");
        return;
    }
    std::printf("  RESULT %-10s %lld frames, %.1f s (%.0f fps), %lld pixel checks, %lld wrong, "
                "%lld failed frames\n",
                name, st.frames, st.seconds, st.frames / (st.seconds > 0 ? st.seconds : 1),
                st.checks, st.wrong, st.failed);
    Check(st.failed == 0, "stress scene: no failed frames");
    Check(st.wrong == 0, "stress scene: geometry checks correct");
}

template <class Draw, class Verify>
SceneStats RunScene(Session& s, const char* name, Draw&& draw, Verify&& verify) {
    SceneStats st;
    const uint64 start = SDL_GetTicks();
    for (long long frame = 0; frame < g_SceneFrames; ++frame) {
        s.window.PollEvents();
        const bool capture = frame % 50 == 0;
        if (capture)
            s.gpu.RequestFrameCapture();
        const auto begun = s.gpu.BeginFrame();
        if (begun == GpuDevice::FrameStart::Skipped)
            continue;
        if (begun != GpuDevice::FrameStart::Ready || !draw(frame) || !s.gpu.EndFrame()) {
            if (st.failed++ < 5)
                std::printf("  %s frame %lld failed: %s\n", name, frame,
                            s.gpu.GetLastError().c_str());
            continue;
        }
        ++st.frames;
        if (capture) {
            GpuFrameCapture c;
            if (!s.gpu.ReadFrameCapture(c)) {
                ++st.failed;
                continue;
            }
            verify(frame, c, st);
        }
        if (!g_QuietScenes && frame > 0 && frame % 1000 == 0)
            std::printf("  %s progress %lld/%lld, wrong %lld, failed %lld\n", name, frame,
                        g_SceneFrames, st.wrong, st.failed);
    }
    st.seconds = (SDL_GetTicks() - start) / 1000.0;
    return st;
}

bool ColourNear(const uint8* p, int r, int g, int b, int tol = 2) {
    return std::abs(p[0] - r) <= tol && std::abs(p[1] - g) <= tol && std::abs(p[2] - b) <= tol;
}

void ParticleStorm(Session& s, uint32 W, uint32 H) {
    // 50 000 rotating particles (6x6), each with its own velocity, wrapped inside the window,
    // drawn in 10 batches. Checked: the centre pixel of 32 random particles is white.
    const int count = g_ParticleCount;
    std::vector<float> px(count), py(count), vx(count), vy(count), spin(count);
    Rng rng(1234);
    for (int i = 0; i < count; ++i) {
        px[i] = rng.Unit() * W, py[i] = rng.Unit() * H;
        vx[i] = (rng.Unit() - .5f) * 8, vy[i] = (rng.Unit() - .5f) * 8,
        spin[i] = (rng.Unit() - .5f) * .3f;
    }
    const float spanX = W - 8.f, spanY = H - 8.f;
    auto position = [&](int i, long long frame, float& x, float& y) {
        x = 4 + std::fmod(std::fmod(px[i] + vx[i] * frame, spanX) + spanX, spanX);
        y = 4 + std::fmod(std::fmod(py[i] + vy[i] * frame, spanY) + spanY, spanY);
    };
    const Mat4 pixels = Mat4::Orthographic(0, float(W), float(H), 0, -1, 1);
    std::vector<RenderVertex> batch;
    auto st = RunScene(
        s, "particles",
        [&](long long frame) {
            bool ok = s.gpu.Clear(0, 0, .15f, 1);
            for (int b = 0; b < 10 && ok; ++b) {
                batch.clear();
                for (int i = b * count / 10; i < (b + 1) * count / 10; ++i) {
                    float x, y;
                    position(i, frame, x, y);
                    PushQuad(batch, x, y, 3, spin[i] * frame, Vec4(1, 1, 1, 1));
                }
                ok = s.gpu.DrawTriangles(batch, pixels);
            }
            return ok;
        },
        [&](long long frame, const GpuFrameCapture& c, SceneStats& st) {
            Rng pick(uint32(frame) + 7);
            for (int k = 0; k < 32; ++k) {
                const int i = int(pick.Next() % count);
                float x, y;
                position(i, frame, x, y);
                ++st.checks;
                if (!ColourNear(Pixel(c, uint32(x), uint32(y)), 255, 255, 255) && st.wrong++ < 5)
                    std::printf("  particles frame %lld: particle %d at (%.1f,%.1f) not drawn\n",
                                frame, i, x, y);
            }
        });
    Report("particles", st);
}

void TilemapScroll(Session& s, uint32 W, uint32 H) {
    // 256x256 tile map (32 px tiles) scrolling diagonally; tiles come from a 4x4 atlas
    // (16 solid colours). One draw call per visible row. Checked: 48 points per capture
    // land on the colour of the tile the scroll offset puts there.
    const int mapSize = 256, tile = 32;
    uint8 palette[16][3];
    for (int i = 0; i < 16; ++i) {
        palette[i][0] = uint8((i & 1) * 200 + 30), palette[i][1] = uint8(((i >> 1) & 1) * 200 + 30);
        palette[i][2] = uint8(((i >> 2) & 1) * 200 + 30 + ((i >> 3) & 1) * 20);
    }
    std::vector<uint8> atlas(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int t = (y / 16) * 4 + x / 16;
            uint8* p = &atlas[(y * 64 + x) * 4];
            p[0] = palette[t][0], p[1] = palette[t][1], p[2] = palette[t][2], p[3] = 255;
        }
    GpuTexture texture;
    if (!texture.Initialize(&s.gpu, 64, 64, atlas.data(), TextureFilter::Nearest))
        return Check(false, "tile atlas");
    std::vector<uint8> map(mapSize * mapSize);
    Rng rng(99);
    for (auto& t : map)
        t = uint8(rng.Next() % 16);
    const int world = mapSize * tile;
    auto scroll = [&](long long frame, int& sx, int& sy) {
        sx = int((frame * 3) % world), sy = int((frame * 2) % world);
    };
    const Mat4 pixels = Mat4::Orthographic(0, float(W), float(H), 0, -1, 1);
    std::vector<RenderVertex> row;
    auto st = RunScene(
        s, "tilemap",
        [&](long long frame) {
            int sx, sy;
            scroll(frame, sx, sy);
            bool ok = s.gpu.Clear(0, 0, 0, 1);
            for (int ty = sy / tile; ty * tile - sy < int(H) && ok; ++ty) {
                row.clear();
                for (int tx = sx / tile; tx * tile - sx < int(W); ++tx) {
                    const int t = map[(ty % mapSize) * mapSize + (tx % mapSize)];
                    const float u0 = ((t % 4) * 16 + .5f) / 64, v0 = ((t / 4) * 16 + .5f) / 64;
                    const float u1 = u0 + 15.f / 64, v1 = v0 + 15.f / 64;
                    const float x0 = float(tx * tile - sx), y0 = float(ty * tile - sy);
                    auto q =
                        Quad(x0, y0, x0 + tile, y0 + tile, 0, Vec4(1, 1, 1, 1), u0, v0, u1, v1);
                    row.insert(row.end(), q.begin(), q.end());
                }
                ok = s.gpu.DrawTriangles(row, pixels, &texture);
            }
            return ok;
        },
        [&](long long frame, const GpuFrameCapture& c, SceneStats& st) {
            int sx, sy;
            scroll(frame, sx, sy);
            Rng pick(uint32(frame) * 31 + 1);
            for (int k = 0; k < 48; ++k) {
                const uint32 x = pick.Next() % W, y = pick.Next() % H;
                const int wx = int(x) + sx, wy = int(y) + sy;
                if (wx % tile < 3 || wx % tile > tile - 4 || wy % tile < 3 || wy % tile > tile - 4)
                    continue; // tile borders are not interesting
                const int t = map[((wy / tile) % mapSize) * mapSize + ((wx / tile) % mapSize)];
                ++st.checks;
                if (!ColourNear(Pixel(c, x, y), palette[t][0], palette[t][1], palette[t][2]) &&
                    st.wrong++ < 5) {
                    const uint8* p = Pixel(c, x, y);
                    std::printf(
                        "  tilemap frame %lld: (%u,%u)=(%u,%u,%u) expected tile %d (%u,%u,%u)\n",
                        frame, x, y, p[0], p[1], p[2], t, palette[t][0], palette[t][1],
                        palette[t][2]);
                }
            }
        });
    Report("tilemap", st);
}

Vec3 TransformPoint(const Mat4& m, const Vec3& v) {
    const double* e = m.elements;
    return Vec3(e[0] * v.x + e[4] * v.y + e[8] * v.z + e[12],
                e[1] * v.x + e[5] * v.y + e[9] * v.z + e[13],
                e[2] * v.x + e[6] * v.y + e[10] * v.z + e[14]);
}

void CubeField(Session& s, uint32 W, uint32 H) {
    // 600 rotating cubes (10x10x6 grid behind) plus a magenta "hero" cube in front of the
    // camera; all depth tested, faces in solid colours. Checked: the hero always occludes
    // the field at the screen centre, and the field shows many different colours.
    const Mat4 view = Mat4::LookAt(Vec3(0, 0, 6), Vec3(0, 0, 0), Vec3(0, 1, 0));
    const Mat4 proj = Mat4::Perspective(1.0f, float(W) / float(H), 0.1f, 100.0f);
    const Mat4 viewProjection = proj * view;
    const Vec3 corners[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                             {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {2, 3, 7, 6}, {0, 3, 7, 4}, {1, 2, 6, 5}};
    std::vector<RenderVertex> verts;
    auto cube = [&](const Mat4& model, Vec4 colour) {
        for (const auto& f : faces) {
            Vec3 p[4];
            for (int k = 0; k < 4; ++k)
                p[k] = TransformPoint(model, corners[f[k]]);
            const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int k : order) {
                RenderVertex v{};
                v.position = p[k];
                v.color = colour;
                v.uv = Vec2(.5f, .5f);
                verts.push_back(v);
            }
        }
    };
    auto st = RunScene(
        s, "cubes",
        [&](long long frame) {
            verts.clear();
            const float t = frame * .02f;
            int index = 0;
            for (int z = 0; z < 6; ++z)
                for (int y = 0; y < 10; ++y)
                    for (int x = 0; x < 10; ++x, ++index) {
                        const Mat4 model =
                            Mat4::Translate(
                                Vec3((x - 4.5f) * 1.2f, (y - 4.5f) * 1.2f, -3.f - z * 2.f)) *
                            Mat4::RotateY(t + index * .1f) * Mat4::RotateX(t * .7f + index * .05f) *
                            Mat4::Scale(Vec3(.4f, .4f, .4f));
                        cube(model, Vec4(((index * 37) % 200 + 40) / 255.f,
                                         ((index * 91) % 200 + 40) / 255.f,
                                         ((index * 53) % 200 + 40) / 255.f, 1));
                    }
            cube(Mat4::Translate(Vec3(0, 0, 2.5f)) * Mat4::RotateY(t * 2) * Mat4::RotateX(t) *
                     Mat4::Scale(Vec3(.6f, .6f, .6f)),
                 Vec4(1, 0, 1, 1));
            return s.gpu.Clear(.05f, .05f, .05f, 1) &&
                   s.gpu.DrawTriangles(verts, viewProjection, nullptr, true);
        },
        [&](long long frame, const GpuFrameCapture& c, SceneStats& st) {
            ++st.checks;
            if (!ColourNear(Pixel(c, W / 2, H / 2), 255, 0, 255) && st.wrong++ < 5) {
                const uint8* p = Pixel(c, W / 2, H / 2);
                std::printf(
                    "  cubes frame %lld: centre (%u,%u,%u), hero cube should hide the field\n",
                    frame, p[0], p[1], p[2]);
            }
            int distinct = 0;
            uint32 seen[64] = {};
            for (uint32 y = 0; y < H && distinct < 64; y += 7)
                for (uint32 x = 0; x < W && distinct < 64; x += 7) {
                    const uint8* p = Pixel(c, x, y);
                    const uint32 key = (p[0] << 16) | (p[1] << 8) | p[2];
                    bool known = false;
                    for (int i = 0; i < distinct; ++i)
                        known |= seen[i] == key;
                    if (!known)
                        seen[distinct++] = key;
                }
            ++st.checks;
            if (distinct < 20 && st.wrong++ < 5)
                std::printf("  cubes frame %lld: only %d colours visible\n", frame, distinct);
        });
    Report("cubes", st);
}

void TextureStreaming(Session& s, uint32 W, uint32 H) {
    // Every frame: 4 new 64x64 textures with frame-dependent colours, drawn into the four
    // quadrants, released right after the frame (still in flight). Checked: quadrant colours.
    const Mat4 pixels = Mat4::Orthographic(0, float(W), float(H), 0, -1, 1);
    auto colour = [](long long frame, int q, int ch) {
        return int((frame * (7 + q * 3) + ch * 85 + q * 50) % 256);
    };
    std::vector<uint8> pixelsData(64 * 64 * 4);
    auto st = RunScene(
        s, "streaming",
        [&](long long frame) {
            GpuTexture textures[4];
            bool ok = s.gpu.Clear(0, 0, 0, 1);
            for (int q = 0; q < 4 && ok; ++q) {
                for (size_t i = 0; i < pixelsData.size(); i += 4) {
                    pixelsData[i] = uint8(colour(frame, q, 0)),
                    pixelsData[i + 1] = uint8(colour(frame, q, 1));
                    pixelsData[i + 2] = uint8(colour(frame, q, 2)), pixelsData[i + 3] = 255;
                }
                ok = textures[q].Initialize(&s.gpu, 64, 64, pixelsData.data(),
                                            TextureFilter::Nearest);
                const float x0 = (q % 2) * W / 2.f, y0 = (q / 2) * H / 2.f;
                auto quad = Quad(x0, y0, x0 + W / 2.f, y0 + H / 2.f, 0, Vec4(1, 1, 1, 1));
                ok = ok && s.gpu.DrawTriangles(quad, pixels, &textures[q]);
            }
            return ok; // textures released here, before EndFrame: the backend must keep them alive
        },
        [&](long long frame, const GpuFrameCapture& c, SceneStats& st) {
            for (int q = 0; q < 4; ++q) {
                const uint32 x = (q % 2) * W / 2 + W / 4, y = (q / 2) * H / 2 + H / 4;
                ++st.checks;
                if (!ColourNear(Pixel(c, x, y), colour(frame, q, 0), colour(frame, q, 1),
                                colour(frame, q, 2)) &&
                    st.wrong++ < 5)
                    std::printf("  streaming frame %lld quadrant %d wrong\n", frame, q);
            }
        });
    Report("streaming", st);
    Check(s.gpu.WaitIdle(), "streaming idle");
    for (int i = 0; i < 4; ++i)
        Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1) && s.gpu.EndFrame();
    s.gpu.WaitIdle();
    Check(s.gpu.GetMemoryStats().liveTextures == 0, "streaming leaves no live textures");
}

void Limits() {
    std::printf("[limits]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    config.maxFrameVertexBytes = 8ull << 20;
    Session s(256, 256, config);
    if (!s.ok)
        return Check(false, "session for limits");
    const Mat4 pixels = Mat4::Orthographic(0, 256, 256, 0, -1, 1);
    auto tiny = Quad(0, 0, 1, 1, 0, Vec4(1, 1, 1, 1));
    // Draw-call limit per frame.
    Check(Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1), "frame for draw limit");
    int accepted = 0;
    while (accepted < 20000 && s.gpu.DrawTriangles(tiny, pixels))
        ++accepted;
    std::printf("  draws accepted in one frame: %d (%s)\n", accepted, s.gpu.GetLastError().c_str());
    Check(accepted == 16384, "draw-call limit is exactly 16384");
    Check(s.gpu.EndFrame() && s.gpu.GetStatus() == GpuStatus::Ready,
          "frame after draw limit completes");
    // Vertex bytes per frame (8 MiB configured).
    std::vector<RenderVertex> big(6 * 30000); // ~8.6 MB
    for (size_t i = 0; i < big.size(); ++i)
        big[i] = tiny[i % 6];
    Check(Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1), "frame for vertex limit");
    Check(!s.gpu.DrawTriangles(big, pixels), "draw above maxFrameVertexBytes rejected");
    std::printf("  vertex limit: %s\n", s.gpu.GetLastError().c_str());
    std::vector<RenderVertex> fits(big.begin(), big.begin() + 6 * 25000); // ~7.2 MB
    Check(s.gpu.DrawTriangles(fits, pixels), "draw below maxFrameVertexBytes accepted");
    Check(s.gpu.EndFrame(), "frame after vertex limit");
    // Texture count.
    std::vector<GpuTexture> many(4200);
    const uint8 px[4] = {1, 2, 3, 255};
    int created = 0;
    for (auto& t : many) {
        if (!t.Initialize(&s.gpu, 1, 1, px))
            break;
        ++created;
    }
    std::printf("  textures created before refusal: %d (%s)\n", created,
                s.gpu.GetLastError().c_str());
    Check(created == 4095, "texture limit (4096 slots, one used by the white texture)");
    many.clear();
    for (int i = 0; i < 4; ++i)
        Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1) && s.gpu.EndFrame();
    s.gpu.WaitIdle();
    GpuTexture again;
    Check(again.Initialize(&s.gpu, 1, 1, px), "textures available again after release");
    again.Release();
    // Big and invalid textures.
    // Large textures: every 16th texel row/column carries its coordinates so we can see which
    // part of the upload arrived. Drawn 1:1 (a 256x256 window onto the texture) at several offsets.
    GpuFrameCapture c;
    for (uint32 size : {1024u, 2048u, 4096u, 8192u}) {
        std::vector<uint8> data(size_t(size) * size * 4);
        for (uint32 y = 0; y < size; ++y)
            for (uint32 x = 0; x < size; ++x) {
                uint8* p = &data[(size_t(y) * size + x) * 4];
                p[0] = uint8(x * 255 / (size - 1)), p[1] = uint8(y * 255 / (size - 1)), p[2] = 200,
                p[3] = 255;
            }
        GpuTexture large;
        const bool created =
            large.Initialize(&s.gpu, size, size, data.data(), TextureFilter::Nearest);
        Check(created, "large texture created");
        int bad = 0;
        for (uint32 oy : {0u, size / 2, size - 256})
            for (uint32 ox : {0u, size / 2, size - 256}) {
                // Map texels [ox, ox+256) x [oy, oy+256) onto the 256x256 window exactly.
                const float u0 = ox / float(size), v0 = oy / float(size),
                            u1 = (ox + 256) / float(size), v1 = (oy + 256) / float(size);
                const bool ok =
                    created && CaptureFrame(s, c, [&] {
                        auto q = Quad(0, 0, 256, 256, 0, Vec4(1, 1, 1, 1), u0, v0, u1, v1);
                        return s.gpu.Clear(0, 0, 0, 1) && s.gpu.DrawTriangles(q, pixels, &large);
                    });
                for (uint32 py : {5u, 128u, 250u})
                    for (uint32 px2 : {5u, 128u, 250u}) {
                        const uint32 tx = ox + px2, ty = oy + py;
                        const uint8* e = &data[(size_t(ty) * size + tx) * 4];
                        if (!ok || !ColourNear(Pixel(c, px2, py), e[0], e[1], e[2], 1)) {
                            if (bad++ < 3)
                                std::printf(
                                    "  %u^2 texel (%u,%u): got (%u,%u,%u) expected (%u,%u,%u)\n",
                                    size, tx, ty, ok ? Pixel(c, px2, py)[0] : 0,
                                    ok ? Pixel(c, px2, py)[1] : 0, ok ? Pixel(c, px2, py)[2] : 0,
                                    e[0], e[1], e[2]);
                        }
                    }
            }
        std::printf("  %ux%u texture: %d wrong texels of 81\n", size, size, bad);
        Check(bad == 0, "large texture uploaded completely");
    }
    GpuTexture invalid;
    Check(!invalid.Initialize(&s.gpu, 16385, 4, nullptr), "16385-wide texture rejected");
    Check(!invalid.Initialize(&s.gpu, 0, 4, nullptr), "zero-size texture rejected");
    Check(s.gpu.GetStatus() == GpuStatus::Ready, "device ready after limits");
}

void TransitionStorm(Session& s) {
    // Random mix of resize, fullscreen toggle, minimize/restore, vsync toggle and captured
    // frames. Every capture must match the window size and the drawn pattern.
    Rng rng(4242);
    long long ops = 0, captures = 0, wrong = 0, failed = 0;
    const long long total = g_SceneFrames;
    for (long long i = 0; i < total; ++i) {
        const uint32 op = rng.Next() % 10;
        if (op == 0) {
            s.window.SetFullscreen(!s.window.IsFullscreen());
        } else if (op == 1) {
            SDL_MinimizeWindow(s.window.GetNativeHandle());
            for (int k = 0; k < 50 && !s.window.IsMinimized(); ++k) {
                s.window.PollEvents();
                SDL_Delay(5);
            }
            // While minimized the backend must skip frames, never fail them.
            if (s.window.IsMinimized() && s.gpu.BeginFrame() != GpuDevice::FrameStart::Skipped)
                ++failed;
            SDL_RestoreWindow(s.window.GetNativeHandle());
            // Restoring is asynchronous (and slow for fullscreen windows): wait for it.
            for (int k = 0; k < 200 && s.window.IsMinimized(); ++k) {
                s.window.PollEvents();
                SDL_Delay(5);
            }
            if (s.window.IsMinimized()) {
                std::printf(
                    "  storm: window manager did not restore the window; leaving fullscreen\n");
                s.window.SetFullscreen(false);
                SDL_RestoreWindow(s.window.GetNativeHandle());
                for (int k = 0; k < 200 && s.window.IsMinimized(); ++k) {
                    s.window.PollEvents();
                    SDL_Delay(5);
                }
            }
        } else if (op == 2) {
            if (!s.gpu.SetVSync(rng.Next() % 2 == 0))
                ++failed;
        } else if (op <= 5 && !s.window.IsFullscreen()) {
            s.window.SetSize(64 + rng.Next() % 1800, 48 + rng.Next() % 1000);
        }
        for (int k = 0; k < 3; ++k)
            s.window.PollEvents();
        ++ops;
        uint32 fw = 0, fh = 0;
        s.window.GetFramebufferSize(fw, fh);
        if (fw < 8 || fh < 8)
            continue;
        if (i % 5 == 0) {
            if (PatternOk(s, fw, fh, "storm"))
                ++captures;
            else if (++wrong > 5)
                break;
        } else if (!(Begin(s.window, s.gpu) && s.gpu.Clear(.2f, .2f, .2f, 1) && s.gpu.EndFrame())) {
            ++failed;
        }
    }
    s.window.SetFullscreen(false);
    s.gpu.SetVSync(false);
    std::printf(
        "  RESULT storm      %lld operations, %lld captured patterns ok, %lld wrong, %lld failed\n",
        ops, captures, wrong, failed);
    Check(wrong == 0 && failed == 0, "transition storm");
    Check(s.gpu.GetStatus() == GpuStatus::Ready, "device ready after storm");
}

// Mixed scenes per window size: particles (10k), tile map, cube field and texture streaming
// rotate in segments until g_FramesPerSize frames were rendered at each size.
void Mix() {
    std::printf("[mix] %lld frames per size\n", g_FramesPerSize);
    GpuDeviceConfig config;
    config.vsync = false;
    Session s(640, 480, config);
    if (!s.ok)
        return Check(false, "session for mix");
    g_QuietScenes = true;
    g_ParticleCount = 10000;
    g_SceneFrames = 1000;

    struct Size {
        uint32 w, h;
        bool fullscreen;
    };

    // Small to large: 21 steps from 160x90 to 1920x1080 (16:9), then fullscreen. The total
    // frame budget (g_FramesPerSize * 1 when --total is used) is split evenly across the steps.
    std::vector<Size> sizes;
    for (uint32 w = 160; w <= 1920; w += 88)
        sizes.push_back({w, w * 9 / 16, false});
    sizes.push_back({0, 0, true});
    if (g_TotalFrames > 0)
        g_FramesPerSize = std::max<long long>(4 * g_SceneFrames,
                                              g_TotalFrames / static_cast<long long>(sizes.size()));
    const double slot = g_RunSeconds > 0 ? g_RunSeconds / sizes.size() : 0;
    if (slot > 0)
        std::printf("  %zu sizes, %.0f s each (%.1f min in total)\n", sizes.size(), slot,
                    g_RunSeconds / 60);
    else
        std::printf("  %zu sizes, %lld frames each\n", sizes.size(), g_FramesPerSize);
    const uint64 begin = SDL_GetTicks();
    for (const Size& size : sizes) {
        if (StopRequested())
            break;
        if (size.fullscreen) {
            s.window.SetFullscreen(true);
            for (int i = 0; i < 100 && !s.window.IsFullscreen(); ++i) {
                s.window.PollEvents();
                SDL_Delay(10);
            }
        } else {
            s.window.SetFullscreen(false);
            s.window.SetSize(size.w, size.h);
        }
        for (int i = 0; i < 30; ++i) {
            s.window.PollEvents();
            SDL_Delay(5);
        }
        uint32 W = 0, H = 0;
        s.window.GetFramebufferSize(W, H);
        std::printf("  == %s %ux%u ==\n", size.fullscreen ? "fullscreen" : "windowed", W, H);
        g_SceneTotals.clear();
        const uint64 start = SDL_GetTicks();
        long long done = 0, nextReport = 20000;
        const long long grand = 0;
        (void)grand;
        auto more = [&] {
            if (slot > 0)
                return (SDL_GetTicks() - start) / 1000.0 < slot;
            return done < g_FramesPerSize;
        };
        while (more() && !StopRequested()) {
            ParticleStorm(s, W, H);
            TilemapScroll(s, W, H);
            CubeField(s, W, H);
            TextureStreaming(s, W, H);
            done += 4 * g_SceneFrames;
            if (done >= nextReport) {
                nextReport += 20000;
                long long wrong = 0, failed = 0, checks = 0;
                for (const auto& [name, st] : g_SceneTotals)
                    wrong += st.wrong, failed += st.failed, checks += st.checks;
                const double secs = (SDL_GetTicks() - start) / 1000.0;
                std::printf("  %ux%u progress %lld/%lld frames, %.0f s, %.0f fps: %lld checks, "
                            "%lld wrong, %lld failed\n",
                            W, H, done, g_FramesPerSize, secs, done / (secs > 0 ? secs : 1), checks,
                            wrong, failed);
            }
        }
        for (const auto& [name, st] : g_SceneTotals)
            std::printf("  RESULT %ux%u %-10s %lld frames, %.0f s (%.0f fps), %lld checks, %lld "
                        "wrong, %lld failed\n",
                        W, H, name.c_str(), st.frames, st.seconds,
                        st.frames / (st.seconds > 0 ? st.seconds : 1), st.checks, st.wrong,
                        st.failed);
    }
    s.window.SetFullscreen(false);
    g_QuietScenes = false;
    std::printf("  mix total %.1f min%s\n", (SDL_GetTicks() - begin) / 60000.0,
                StopRequested() ? " (stopped early)" : "");
}

// Grow: the window gets bigger every second (160x90 -> 1920x1080 in 80 px steps, then 3 s
// fullscreen, then starts small again) until the deadline, while every frame renders all
// scenes at once: scrolling tile map, 20 000 rotating particles, 300 rotating cubes plus a
// hero cube (depth tested), rotating textured sprites and 4 freshly streamed textures.
float g_GrowSpeed = 1.0f;
int g_GrowStepMs = 1000;     // window grows one step this often
int g_GrowSpawnPerFrame = 0; // 0 = everything from the first frame; else objects appear this fast
int g_GrowParticles = 20000, g_GrowCubeLayers = 3, g_GrowSprites = 40, g_GrowOverdraw = 0;

void Grow() {
    std::printf("[grow] until deadline (%.0f s): %d particles, %d cubes, %d sprites, %d overdraw "
                "layers, speed x%.0f\n",
                g_RunSeconds, g_GrowParticles, g_GrowCubeLayers * 100, g_GrowSprites,
                g_GrowOverdraw, g_GrowSpeed);
    GpuDeviceConfig config;
    config.vsync = false;
    config.maxFrameVertexBytes = 512ull << 20;
    Session s(160, 90, config);
    if (!s.ok)
        return Check(false, "session for grow");
    // Tile atlas: 16 solid colours.
    uint8 palette[16][3];
    for (int i = 0; i < 16; ++i) {
        palette[i][0] = uint8((i & 1) * 160 + 40), palette[i][1] = uint8(((i >> 1) & 1) * 160 + 40);
        palette[i][2] = uint8(((i >> 2) & 1) * 160 + 40 + ((i >> 3) & 1) * 30);
    }
    std::vector<uint8> atlasData(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const int t = (y / 16) * 4 + x / 16;
            uint8* p = &atlasData[(y * 64 + x) * 4];
            p[0] = palette[t][0], p[1] = palette[t][1], p[2] = palette[t][2], p[3] = 255;
        }
    GpuTexture atlas, sprite;
    const uint8 checker[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    if (!atlas.Initialize(&s.gpu, 64, 64, atlasData.data(), TextureFilter::Nearest) ||
        !sprite.Initialize(&s.gpu, 2, 2, checker, TextureFilter::Nearest))
        return Check(false, "grow textures");
    std::vector<uint8> map(128 * 128);
    Rng rng(7);
    for (auto& t : map)
        t = uint8(rng.Next() % 16);
    const int particles = g_GrowParticles;
    std::vector<float> px(particles), py(particles), vx(particles), vy(particles), spin(particles);
    for (int i = 0; i < particles; ++i) {
        px[i] = rng.Unit(), py[i] = rng.Unit();
        vx[i] = (rng.Unit() - .5f) * .004f * g_GrowSpeed,
        vy[i] = (rng.Unit() - .5f) * .004f * g_GrowSpeed;
        spin[i] = (rng.Unit() - .5f) * .3f * g_GrowSpeed;
    }
    const Vec3 corners[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                             {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 5, 4},
                             {2, 3, 7, 6}, {0, 3, 7, 4}, {1, 2, 6, 5}};
    std::vector<RenderVertex> tiles, dots, cubes, sprites, overdraw;
    std::vector<uint8> stream(32 * 32 * 4);
    auto addCube = [&](const Mat4& model, Vec4 colour) {
        for (const auto& f : faces) {
            Vec3 q[4];
            for (int k = 0; k < 4; ++k)
                q[k] = TransformPoint(model, corners[f[k]]);
            for (int k : {0, 1, 2, 0, 2, 3}) {
                RenderVertex v{};
                v.position = q[k], v.color = colour, v.uv = Vec2(.5f, .5f);
                cubes.push_back(v);
            }
        }
    };
    auto streamColour = [](long long frame, int q, int ch) {
        return int((frame * (5 + q * 3) + ch * 85 + q * 60) % 256);
    };

    // Size schedule.
    std::vector<std::pair<uint32, uint32>> sizes;
    for (uint32 w = 160; w <= 1920; w += 80)
        sizes.push_back({w, w * 9 / 16});
    const int fullscreenSeconds = 3;
    const uint64 begin = SDL_GetTicks();
    long long frame = 0, failed = 0, checks = 0, wrong = 0, cycles = 0, sizeChanges = 0;
    size_t step = 0;
    int fullLeft = 0;
    long long spawnFrame = 0;
    uint64 nextStep = begin;
    uint64 stepFrames = 0, stepStart = begin;
    uint32 W = 0, H = 0;
    while ((SDL_GetTicks() - begin) / 1000.0 < g_RunSeconds && !StopRequested()) {
        const uint64 now = SDL_GetTicks();
        if (now >= nextStep) {
            if (W && stepFrames)
                std::printf("  %4ux%-4u %s: %llu frames in %.1f s (%.0f fps), checks %lld, wrong "
                            "%lld, failed %lld\n",
                            W, H, s.window.IsFullscreen() ? "full" : "win ",
                            static_cast<unsigned long long>(stepFrames), (now - stepStart) / 1000.0,
                            stepFrames * 1000.0 / std::max<uint64>(1, now - stepStart), checks,
                            wrong, failed);
            nextStep = now + static_cast<uint64>(g_GrowStepMs);
            stepFrames = 0, stepStart = now;
            ++sizeChanges;
            if (fullLeft > 0) {
                if (--fullLeft == 0) {
                    s.window.SetFullscreen(false);
                    step = 0;
                    ++cycles;
                    spawnFrame = frame; // objects are created again from zero every cycle
                }
            } else if (step < sizes.size()) {
                s.window.SetSize(sizes[step].first, sizes[step].second);
                ++step;
            } else {
                s.window.SetFullscreen(true);
                fullLeft = fullscreenSeconds;
            }
            if (!s.window.IsFullscreen() && fullLeft == 0 && step > 0)
                s.window.SetSize(sizes[step - 1].first, sizes[step - 1].second);
        }
        s.window.PollEvents();
        s.window.GetFramebufferSize(W, H);
        if (W < 16 || H < 16)
            continue;
        const float fw = float(W), fh = float(H);
        const Mat4 pixels = Mat4::Orthographic(0, fw, fh, 0, -1, 1);
        // Tile map scrolling (32 px tiles).
        tiles.clear();
        const int tile = 32, sx = int((frame * 3) % (128 * tile)),
                  sy = int((frame * 2) % (128 * tile));
        for (int ty = sy / tile; ty * tile - sy < int(H); ++ty)
            for (int tx = sx / tile; tx * tile - sx < int(W); ++tx) {
                const int t = map[(ty % 128) * 128 + (tx % 128)];
                const float u0 = ((t % 4) * 16 + .5f) / 64, v0 = ((t / 4) * 16 + .5f) / 64;
                const float x0 = float(tx * tile - sx), y0 = float(ty * tile - sy);
                auto q = Quad(x0, y0, x0 + tile, y0 + tile, 0, Vec4(1, 1, 1, 1), u0, v0,
                              u0 + 15.f / 64, v0 + 15.f / 64);
                tiles.insert(tiles.end(), q.begin(), q.end());
            }
        // Spawning: live object counts ramp up by g_GrowSpawnPerFrame every frame.
        const long long alive =
            g_GrowSpawnPerFrame > 0 ? (frame - spawnFrame + 1) * g_GrowSpawnPerFrame : (1ll << 40);
        const int liveParticles = static_cast<int>(std::min<long long>(particles, alive));
        const int liveSprites =
            static_cast<int>(std::min<long long>(g_GrowSprites, alive / 10 + 40));
        const int liveLayers =
            static_cast<int>(std::min<long long>(g_GrowCubeLayers, alive / 5000 + 1));
        // Particles (positions relative to the window so they fill it at every size).
        dots.clear();
        for (int i = 0; i < liveParticles; ++i) {
            const float x =
                3 + std::fmod(std::fmod(px[i] + vx[i] * frame, 1.f) + 1.f, 1.f) * (fw - 6);
            const float y =
                3 + std::fmod(std::fmod(py[i] + vy[i] * frame, 1.f) + 1.f, 1.f) * (fh - 6);
            PushQuad(dots, x, y, 2.5f, spin[i] * frame, Vec4(1, 1, 1, 1));
        }
        // Cubes.
        cubes.clear();
        const float t = frame * .02f * g_GrowSpeed;
        int index = 0;
        for (int z = 0; z < liveLayers; ++z)
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < 10; ++x, ++index)
                    addCube(
                        Mat4::Translate(
                            Vec3((x - 4.5f) * 1.3f, (y - 4.5f) * 1.3f, -3.f - z * .8f)) *
                            Mat4::RotateY(t + index * .1f) * Mat4::RotateX(t * .7f + index * .05f) *
                            Mat4::Scale(Vec3(.35f, .35f, .35f)),
                        Vec4(((index * 37) % 200 + 40) / 255.f, ((index * 91) % 200 + 40) / 255.f,
                             ((index * 53) % 200 + 40) / 255.f, 1));
        addCube(Mat4::Translate(Vec3(0, 0, 2.5f)) * Mat4::RotateY(t * 2) * Mat4::RotateX(t) *
                    Mat4::Scale(Vec3(.6f, .6f, .6f)),
                Vec4(1, 0, 1, 1));
        const Mat4 viewProjection = Mat4::Perspective(1.0f, fw / fh, .1f, 100.f) *
                                    Mat4::LookAt(Vec3(0, 0, 6), Vec3(0, 0, 0), Vec3(0, 1, 0));
        // Rotating textured sprites along the bottom edge.
        sprites.clear();
        const float spriteHalf = std::max(4.f, fh / 20);
        // Sprites: one row along the bottom edge, the rest flying around the window.
        for (int i = 0; i < liveSprites; ++i) {
            if (i < 40) {
                PushQuad(sprites, (i + .5f) * fw / 40, fh - spriteHalf - 2, spriteHalf,
                         t * (i % 2 ? 1 : -1) + i, Vec4(1, 1, 1, 1));
            } else {
                const float a = i * 2.39996f + t * (0.3f + (i % 7) * .05f);
                const float r = .45f * std::fmod(i * .618034f, 1.f);
                PushQuad(sprites, fw * (.5f + r * std::cos(a)), fh * (.5f + r * std::sin(a)),
                         spriteHalf * (.5f + std::fmod(i * .37f, 1.f)), t * ((i % 5) - 2) * .5f + i,
                         Vec4(1, 1, 1, .8f));
            }
        }
        // Overdraw: full-window translucent layers (fill-rate load).
        overdraw.clear();
        for (int l = 0; l < g_GrowOverdraw; ++l) {
            auto q = Quad(0, 0, fw, fh, 0,
                          Vec4((l % 3) / 2.f, ((l + 1) % 3) / 2.f, ((l + 2) % 3) / 2.f, .02f));
            overdraw.insert(overdraw.end(), q.begin(), q.end());
        }

        const bool capture = frame % 30 == 0;
        if (capture)
            s.gpu.RequestFrameCapture();
        const auto begun = s.gpu.BeginFrame();
        if (begun == GpuDevice::FrameStart::Skipped)
            continue;
        GpuTexture streamed[4];
        const float box = std::max(8.f, std::min(fw, fh) / 8);
        bool ok = begun == GpuDevice::FrameStart::Ready && s.gpu.Clear(0, 0, 0, 1) &&
                  s.gpu.DrawTriangles(tiles, pixels, &atlas) &&
                  (overdraw.empty() || s.gpu.DrawTriangles(overdraw, pixels)) &&
                  s.gpu.DrawTriangles(dots, pixels) &&
                  s.gpu.DrawTriangles(sprites, pixels, &sprite) &&
                  // 2D layers write no depth: the opaque cubes drawn after them stay on top
                  s.gpu.DrawTriangles(cubes, viewProjection, nullptr, true);
        for (int q = 0; q < 4 && ok; ++q) {
            for (size_t i = 0; i < stream.size(); i += 4)
                stream[i] = uint8(streamColour(frame, q, 0)),
                stream[i + 1] = uint8(streamColour(frame, q, 1)),
                stream[i + 2] = uint8(streamColour(frame, q, 2)), stream[i + 3] = 255;
            ok = streamed[q].Initialize(&s.gpu, 32, 32, stream.data(), TextureFilter::Nearest);
            const float x0 = q % 2 ? fw - box : 0, y0 = q / 2 ? fh * .5f : 0;
            auto quad = Quad(x0, y0, x0 + box, y0 + box, 0, Vec4(1, 1, 1, 1));
            ok = ok && s.gpu.DrawTriangles(quad, pixels, &streamed[q]);
        }
        if (begun == GpuDevice::FrameStart::Ready)
            ok = s.gpu.EndFrame() && ok;
        if (!ok) {
            if (failed++ < 5)
                std::printf("  frame %lld at %ux%u failed: %s\n", frame, W, H,
                            s.gpu.GetLastError().c_str());
            ++frame;
            continue;
        }
        ++stepFrames;
        if (capture) {
            GpuFrameCapture c;
            if (!s.gpu.ReadFrameCapture(c)) {
                ++failed;
            } else if (c.width == W && c.height == H) {
                // Hero cube in the centre, streamed textures in their corners.
                ++checks;
                bool good = ColourNear(Pixel(c, W / 2, H / 2), 255, 0, 255);
                for (int q = 0; q < 4; ++q) {
                    const uint32 x = uint32((q % 2 ? fw - box / 2 : box / 2)),
                                 y = uint32((q / 2 ? fh * .5f : 0) + box / 2);
                    good = good && ColourNear(Pixel(c, std::min(x, W - 1), std::min(y, H - 1)),
                                              streamColour(frame, q, 0), streamColour(frame, q, 1),
                                              streamColour(frame, q, 2));
                }
                if (!good && wrong++ < 5) {
                    const uint8* p = Pixel(c, W / 2, H / 2);
                    std::printf("  frame %lld at %ux%u: check failed (centre %u,%u,%u)\n", frame, W,
                                H, p[0], p[1], p[2]);
                }
            }
        }
        ++frame;
    }
    s.window.SetFullscreen(false);
    std::printf("  RESULT grow: %lld frames, %lld size steps, %lld full cycles, %lld checks, %lld "
                "wrong, %lld failed, %.1f min\n",
                frame, sizeChanges, cycles, checks, wrong, failed,
                (SDL_GetTicks() - begin) / 60000.0);
    Check(failed == 0, "grow: no failed frames");
    Check(wrong == 0, "grow: checks correct");
}

void Stress() {
    std::printf("[stress] %lld frames per scene\n", g_SceneFrames);
    Limits();
    GpuDeviceConfig config;
    config.vsync = false;
    Session s(1280, 720, config);
    if (!s.ok)
        return Check(false, "session for stress");
    const uint32 sizes[][2] = {{640, 480}, {1280, 720}, {1920, 1080}};
    for (const auto& size : sizes) {
        s.window.SetSize(size[0], size[1]);
        for (int i = 0; i < 20; ++i) {
            s.window.PollEvents();
            SDL_Delay(5);
        }
        uint32 W = 0, H = 0;
        s.window.GetFramebufferSize(W, H);
        std::printf("  == %ux%u ==\n", W, H);
        ParticleStorm(s, W, H);
        TilemapScroll(s, W, H);
        CubeField(s, W, H);
        TextureStreaming(s, W, H);
    }
    std::printf("  == transition storm ==\n");
    TransitionStorm(s);
}
} // namespace

// HDR scene renderer (Renderer3D in PBR mode on the Forge backend): same checks as the Vulkan
// suites pbr/hdr/shadows, plus the Forge-specific target lifetime and environment threading.
std::array<int, 4> At(const GpuFrameCapture& c, float fx, float fy) {
    const uint32 x = std::min(c.width - 1, static_cast<uint32>(fx * c.width));
    const uint32 y = std::min(c.height - 1, static_cast<uint32>(fy * c.height));
    const uint8* p = Pixel(c, x, y);
    return {p[0], p[1], p[2], p[3]};
}

void Scene3D() {
    std::printf("[scene3d]\n");
    GpuDeviceConfig config;
    config.vsync = false; // The shutdown latency check must not measure display pacing.
    Session s(128, 128, config);
    if (!s.ok)
        return Check(false, "session for scene3d");
    Renderer3D::Context context(&s.gpu);
    Renderer3D::ScopedContext scoped(context);
    PerspectiveCamera camera(ToRadians(60), 1, .1f, 10);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    const std::array<Vertex3D, 4> vertices{{{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
                                            {{1, -1, 0}, {0, 0, 1}, {1, 0}},
                                            {{1, 1, 0}, {0, 0, 1}, {1, 1}},
                                            {{-1, 1, 0}, {0, 0, 1}, {0, 1}}}};
    const std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D plane;
    Check(plane.Upload(&s.gpu, vertices, indices), "scene plane uploads");
    GpuFrameCapture c;

    std::vector<RenderVertex> cubeVertices;
    std::vector<uint32> cubeIndices;
    CubeMesh::Build({1, 1, 1, 1}, cubeVertices, cubeIndices);
    std::vector<Vertex3D> orientationVertices;
    for (const auto& vertex : cubeVertices)
        orientationVertices.push_back({vertex.position, vertex.normal, vertex.uv});
    Mesh3D orientationCube;
    Check(orientationCube.Upload(&s.gpu, orientationVertices, cubeIndices),
          "orientation cube uploads");
    for (int path = 0; path < 3; ++path) {
        const auto mode = path == 2 ? Render3DMode::Pbr : Render3DMode::Legacy;
        const char* label = path == 0   ? "procedural cube"
                            : path == 1 ? "migrated scene mesh"
                                        : "PBR scene mesh";
        const bool captured = CaptureFrame(s, c, [&] {
            s.gpu.Clear(0, 0, 0, 1);
            Render3DSettings settings;
            settings.mode = mode;
            settings.sun.direction = {0, 0, -1};
            settings.shadows = false;
            settings.imageBasedLighting = false;
            Renderer3D::BeginScene(camera, settings, 1);
            Material3D red, green;
            red.albedoColor = {1, 0, 0, 1};
            green.albedoColor = {0, 1, 0, 1};
            red.metallic = green.metallic = 1;
            red.roughness = green.roughness = 1;
            if (path == 0) {
                Renderer3D::DrawCube({0, .75f, 0}, {.44f, .44f, .44f}, red);
                Renderer3D::DrawCube({0, -.75f, 0}, {.44f, .44f, .44f}, green);
            } else {
                Renderer3D::DrawMesh(
                    orientationCube,
                    Mat4::Translate({0, .75f, 0}) * Mat4::Scale({.22f, .22f, .22f}), red);
                Renderer3D::DrawMesh(
                    orientationCube,
                    Mat4::Translate({0, -.75f, 0}) * Mat4::Scale({.22f, .22f, .22f}), green);
            }
            Renderer3D::EndScene();
            return true;
        });
        Check(captured, "vertical orientation frame captured");
        if (!captured)
            continue;
        const auto top = At(c, .5f, .28f);
        const auto bottom = At(c, .5f, .72f);
        std::printf("  %s orientation: top=%d,%d,%d bottom=%d,%d,%d\n", label, top[0], top[1],
                    top[2], bottom[0], bottom[1], bottom[2]);
        Check(top[0] > 20 && top[0] > top[1] && bottom[1] > 20 && bottom[1] > bottom[0],
              "PBR geometry places positive world Y above negative world Y");
    }

    // PBR materials (Engine_pbr).
    auto material = [&](float metallic, float roughness, bool modern) {
        Check(CaptureFrame(s, c,
                           [&] {
                               s.gpu.Clear(0, 0, 0, 1);
                               Render3DSettings r;
                               r.mode = modern ? Render3DMode::Pbr : Render3DMode::Legacy;
                               r.sun.direction = {0, 0, -1};
                               r.shadows = false;
                               r.imageBasedLighting = false;
                               Renderer3D::BeginScene(camera, r, 1);
                               Material3D m;
                               m.albedoColor = {.3f, .1f, .03f, 1};
                               m.metallic = metallic;
                               m.roughness = roughness;
                               Renderer3D::DrawMesh(plane, Mat4{}, m);
                               Renderer3D::EndScene();
                               return true;
                           }),
              "PBR frame captured");
        return At(c, .5f, .5f);
    };
    const auto dielectric = material(0, .65f, true);
    Check(context.GetLastAssetError().empty(), "HDR scene renders without fallback");
    if (!context.GetLastAssetError().empty())
        std::printf("  scene error: %s\n", context.GetLastAssetError().c_str());
    const auto metal = material(1, .65f, true);
    const auto glossy = material(0, .2f, true);
    std::printf("  dielectric=%d,%d metal=%d,%d glossy=%d\n", dielectric[0], dielectric[1],
                metal[0], metal[1], glossy[0]);
    Check(dielectric[0] > 5, "lit dielectric is visible");
    Check(dielectric != metal, "metallic changes the GPU output");
    Check(glossy[0] > dielectric[0] + 10, "roughness changes the specular highlight");
    Check(material(1, .65f, false) == metal,
          "legacy compatibility input uses PBR metallic and roughness");

    // Untouched pixels keep the clear colour; a 2D overlay after the scene keeps its colour.
    Check(CaptureFrame(s, c,
                       [&] {
                           s.gpu.Clear(0, 0, 1, 1);
                           Render3DSettings r;
                           r.mode = Render3DMode::Pbr;
                           r.imageBasedLighting = false;
                           r.shadows = false;
                           Renderer3D::BeginScene(camera, r, 1);
                           Renderer3D::DrawCube({0, 0, 0}, {.5f, .5f, .5f}, {});
                           Renderer3D::EndScene();
                           const std::array<RenderVertex, 3> overlay{
                               {{{-1, -1, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{-.6f, -1, 0}, {0, 1, 0, 1}, {0, 0}},
                                {{-1, -.6f, 0}, {0, 1, 0, 1}, {0, 0}}}};
                           return s.gpu.DrawTriangles(overlay, Mat4{});
                       }),
          "scene with overlay captured");
    const auto corner = At(c, .98f, .02f);
    Check(corner[0] == 0 && corner[1] == 0 && corner[2] == 255,
          "background outside the scene keeps the clear colour");
    const auto centre = At(c, .5f, .5f);
    Check(centre != corner, "cube covers the centre");
    std::printf("  centre=%d,%d,%d corner=%d,%d,%d\n", centre[0], centre[1], centre[2], corner[0],
                corner[1], corner[2]);
    int greens = 0;
    for (size_t i = 0; i < c.rgba.size(); i += 4)
        greens += c.rgba[i] == 0 && c.rgba[i + 1] == 255 && c.rgba[i + 2] == 0;
    Check(greens > 20, "2D overlay drawn after tone mapping keeps its colour");

    // Shadows (Engine_shadows) and shadow map lifetime (#36).
    PerspectiveCamera view(ToRadians(55), 1, .1f, 100);
    view.SetPosition({6, 7, 9});
    view.SetTarget({0, 1, 0});
    Render3DSettings shadowed;
    shadowed.mode = Render3DMode::Pbr;
    shadowed.imageBasedLighting = false;
    shadowed.environmentColor = {.04f, .04f, .04f};
    shadowed.sun.direction = {.7f, -1, .3f};
    shadowed.sun.intensity = 3;
    shadowed.shadows = false;
    Material3D ground;
    ground.albedoColor = {.5f, .5f, .5f, 1};
    ground.roughness = .7f;
    Material3D object;
    object.albedoColor = {.6f, .15f, .04f, 1};
    object.roughness = .4f;
    auto shadowFrame = [&](float shift = 0) {
        GpuFrameCapture out;
        Check(CaptureFrame(s, out,
                           [&] {
                               s.gpu.Clear(.03f, .04f, .05f, 1);
                               Renderer3D::BeginScene(view, shadowed, 1);
                               Renderer3D::DrawCube({0, -.1f, 0}, {10, .2f, 10}, ground);
                               Renderer3D::DrawCube({shift, 1, 0}, {1, 2, 1}, object);
                               Renderer3D::EndScene();
                               return true;
                           }),
              "shadow frame captured");
        return out;
    };
    const auto lit = shadowFrame();
    const uint64 noShadowBytes = s.gpu.GetMemoryStats().allocationBytes;
    shadowed.shadows = true;
    const auto dark = shadowFrame();
    Check(context.GetLastAssetError().empty(), "shadow pass renders without fallback");
    size_t darker = 0;
    for (size_t i = 0; i < lit.rgba.size(); i += 4)
        darker += lit.rgba[i] > dark.rgba[i] + 20;
    std::printf("  shadow darkened_pixels=%zu\n", darker);
    Check(darker > 50, "directional shadow darkens receiver pixels");
    Check(shadowFrame(2).rgba != dark.rgba, "moving the caster moves the shadow");
    shadowed.shadowResolution = 4096;
    shadowFrame();
    const uint64 bigShadowBytes = s.gpu.GetMemoryStats().allocationBytes;
    Check(bigShadowBytes >= noShadowBytes + 4096ull * 4096 * 4, "4096 shadow map is accounted");
    shadowed.shadows = false;
    for (int i = 0; i < 4; ++i)
        shadowFrame();
    std::printf("  scene bytes: no shadow %llu, 4096 shadow %llu, after off %llu\n",
                static_cast<unsigned long long>(noShadowBytes),
                static_cast<unsigned long long>(bigShadowBytes),
                static_cast<unsigned long long>(s.gpu.GetMemoryStats().allocationBytes));
    Check(s.gpu.GetMemoryStats().allocationBytes == noShadowBytes,
          "turning shadows off releases the shadow map");
    const uint64 before = s.gpu.GetMemoryStats().allocationBytes;
    for (int i = 0; i < 40; ++i) {
        shadowed.shadows = (i % 2) == 0;
        shadowFrame();
    }
    shadowed.shadows = false;
    for (int i = 0; i < 4; ++i)
        shadowFrame();
    Check(s.gpu.GetMemoryStats().allocationBytes == before,
          "shadow toggles leave no allocations behind");

    // Image-based lighting and sky (Engine_hdr): the procedural sky of an empty path.
    Render3DSettings sky;
    sky.mode = Render3DMode::Pbr;
    sky.shadows = false;
    sky.showSkybox = true;
    sky.imageBasedLighting = true;
    auto skyFrame = [&] {
        GpuFrameCapture out;
        CaptureFrame(s, out, [&] {
            s.gpu.Clear(1, 0, 1, 1);
            Renderer3D::BeginScene(camera, sky, 1);
            Material3D m;
            m.albedoColor = {.8f, .8f, .8f, 1};
            m.roughness = .3f;
            Renderer3D::DrawMesh(plane, Mat4::Scale({.3f, .3f, .3f}), m);
            Renderer3D::EndScene();
            return true;
        });
        return out;
    };
    const auto start = SDL_GetTicks();
    while (!context.IsEnvironmentReady() && SDL_GetTicks() - start < 20000)
        skyFrame();
    Check(context.IsEnvironmentReady(), "environment prepared in the background");
    const auto withSky = skyFrame();
    const auto skyCorner = At(withSky, .02f, .02f);
    std::printf("  sky corner=%d,%d,%d centre=%d,%d,%d\n", skyCorner[0], skyCorner[1], skyCorner[2],
                At(withSky, .5f, .5f)[0], At(withSky, .5f, .5f)[1], At(withSky, .5f, .5f)[2]);
    Check(!(skyCorner[0] == 255 && skyCorner[1] == 0 && skyCorner[2] == 255),
          "sky replaces the clear colour");
    Check(context.GetLastAssetError().empty(), "IBL scene renders without errors");
    sky.showSkybox = false;
    const auto noSky = skyFrame();
    const auto plain = At(noSky, .02f, .02f);
    Check(plain[0] == 255 && plain[1] == 0 && plain[2] == 255,
          "without the sky the clear colour stays");
    const auto iblCentre = At(noSky, .5f, .5f);
    Check(iblCentre[0] > 20, "image-based lighting lights the plane");

    // Renderer shutdown while an environment is still being prepared never waits for it (#38).
    {
        Renderer3D::Context other(&s.gpu);
        Renderer3D::ScopedContext otherScope(other);
        Render3DSettings fresh = sky;
        fresh.environmentRotation = 10;
        CaptureFrame(s, c, [&] {
            s.gpu.Clear(0, 0, 0, 1);
            Renderer3D::BeginScene(camera, fresh, 1);
            Renderer3D::DrawCube({0, 0, 0}, {.5f, .5f, .5f}, {});
            Renderer3D::EndScene();
            return true;
        });
        other.Shutdown();
    }
    const auto quick = SDL_GetTicksNS();
    for (int i = 0; i < 4; ++i)
        Check(Begin(s.window, s.gpu) && s.gpu.Clear(0, 0, 0, 1) && s.gpu.EndFrame(),
              "frame after shutdown");
    const double ms = (SDL_GetTicksNS() - quick) / 1e6;
    std::printf("  4 frames after shutdown during preparation: %.1f ms\n", ms);
    Check(ms < 250, "shutdown during environment preparation does not stall later frames");

    // Window resize: the HDR target follows the swap chain.
    Check(s.window.SetSize(200, 150), "window resized for the scene");
    s.window.PollEvents();
    Check(CaptureFrame(s, c,
                       [&] {
                           s.gpu.Clear(0, 0, 1, 1);
                           Render3DSettings r;
                           r.mode = Render3DMode::Pbr;
                           r.imageBasedLighting = false;
                           r.shadows = false;
                           Renderer3D::BeginScene(camera, r, 1);
                           Renderer3D::DrawCube({0, 0, 0}, {.5f, .5f, .5f}, {});
                           Renderer3D::EndScene();
                           return true;
                       }),
          "scene after resize captured");
    std::printf("  after resize: %ux%u\n", c.width, c.height);
    Check(c.width == 200 && c.height == 150, "capture follows the new window size");
    Check(At(c, .5f, .5f) != At(c, .02f, .02f) && At(c, .02f, .02f)[2] == 255,
          "scene drawn at the new size");
    Check(context.GetLastAssetError().empty(), "no scene errors after resize");
}

void MaterialModes() {
    Session session(96, 96);
    if (!session.ok)
        return Check(false, "material session initializes");
    Renderer3D::Context context(&session.gpu);
    Renderer3D::ScopedContext scoped(context);
    PerspectiveCamera camera(ToRadians(60), 1, .1f, 10);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    const std::array<Vertex3D, 4> vertices{{{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
                                            {{1, -1, 0}, {0, 0, 1}, {1, 0}},
                                            {{1, 1, 0}, {0, 0, 1}, {1, 1}},
                                            {{-1, 1, 0}, {0, 0, 1}, {0, 1}}}};
    const std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D plane;
    Check(plane.Upload(&session.gpu, vertices, indices), "material plane uploads");
    for (auto renderMode : {Render3DMode::Legacy, Render3DMode::Pbr}) {
        Render3DSettings settings;
        settings.mode = renderMode;
        settings.shadows = false;
        settings.imageBasedLighting = false;
        settings.toneMapping = false;
        settings.sun.direction = {0, 0, -1};
        const auto render = [&](Material3D nearMaterial, Material3D farMaterial, bool reverse) {
            GpuFrameCapture capture;
            Check(CaptureFrame(session, capture,
                               [&] {
                                   session.gpu.Clear(0, 0, 1, 1);
                                   Renderer3D::BeginScene(camera, settings, 1);
                                   const auto nearDraw = [&] {
                                       Renderer3D::DrawMesh(plane, Mat4::Translate({0, 0, .25f}),
                                                            nearMaterial);
                                   };
                                   const auto farDraw = [&] {
                                       Renderer3D::DrawMesh(plane, Mat4::Translate({0, 0, -.25f}),
                                                            farMaterial);
                                   };
                                   if (reverse) {
                                       nearDraw();
                                       farDraw();
                                   } else {
                                       farDraw();
                                       nearDraw();
                                   }
                                   Renderer3D::EndScene();
                                   return true;
                               }),
                  "material frame captures");
            return At(capture, .5f, .5f);
        };
        Material3D red, green;
        red.albedoColor = {1, 0, 0, .5f};
        green.albedoColor = {0, 1, 0, .5f};
        red.metallic = green.metallic = 1;
        red.roughness = green.roughness = 1;
        red.alphaMode = green.alphaMode = MaterialAlphaMode::Blend;
        const auto forward = render(red, green, false);
        const auto reverse = render(red, green, true);
        Check(forward == reverse && forward[0] > 10 && forward[1] > 10 && forward[2] > 10,
              "transparent layers sort independently of insertion order");
        red.alphaMode = MaterialAlphaMode::Opaque;
        const auto opaque = render(red, green, true);
        Check(opaque[0] > 10 && opaque[1] < 3 && opaque[2] < 3,
              "opaque material writes depth and ignores alpha");
        red.alphaMode = MaterialAlphaMode::Mask;
        red.alphaCutoff = .75f;
        const auto masked = render(red, green, true);
        Check(masked[0] < 3 && masked[1] > 10 && masked[2] > 10,
              "mask cutoff exposes geometry behind it");
        red.alphaCutoff = .25f;
        Check(render(red, green, true)[1] < 3, "mask passing cutoff writes depth");
        red.alphaMode = MaterialAlphaMode::Blend;
        green.alphaMode = MaterialAlphaMode::Opaque;
        green.albedoColor.w = 1;
        red.albedoColor.w = 0;
        const auto zero = render(red, green, false);
        Check(zero[0] < 3 && zero[1] > 10 && zero[2] < 3,
              "zero-alpha blend preserves opaque background");
        Check(context.GetLastAssetError().empty(), "material renderer reports no errors");
    }
}

void MaterialTextures() {
    std::printf("[material textures]\n");
    Session session(96, 96);
    if (!session.ok)
        return Check(false, "texture session initializes");
    for (const auto dimensions : {std::array<uint32, 2>{1, 1}, {1, 9}, {13, 1}, {7, 5}}) {
        const auto [width, height] = dimensions;
        std::vector<uint8> pixels(static_cast<size_t>(width) * height * 4, 255);
        GpuTexture texture;
        TextureSampling sampling{true, TextureAddressMode::Repeat, true, false};
        Check(texture.Initialize(&session.gpu, width, height, pixels.data(), TextureFilter::Linear,
                                 TextureColorSpace::Srgb, sampling),
              "odd or one-dimensional mip texture uploads");
        Check(texture.GetMipCount() > 0 && texture.GetStorageBytes() >= pixels.size(),
              "GPU mip accounting is exposed");
        std::vector<TextureMipLevel> levels;
        Check(BuildTextureMipChain(width, height, pixels, TextureColorSpace::Srgb, false, levels) &&
                  texture.GetMipCount() == levels.size(),
              "GPU receives every CPU mip level");
        std::vector<RenderVertex> quad;
        PushQuad(quad, 48, 48, 24, 0, {1, 1, 1, 1});
        GpuFrameCapture capture;
        Check(CaptureFrame(session, capture,
                           [&] {
                               session.gpu.Clear(0, 0, 0, 1);
                               return session.gpu.DrawTriangles(
                                   quad, Mat4::Orthographic(0, 96, 96, 0, -1, 1), &texture);
                           }),
              "one-dimensional mip texture renders");
        const auto center = At(capture, .5f, .5f);
        Check(center[0] > 250 && center[1] > 250 && center[2] > 250,
              "one-dimensional mip texture samples valid white texels");
    }
    Renderer3D::Context context(&session.gpu);
    Renderer3D::ScopedContext scoped(context);
    PerspectiveCamera camera(ToRadians(60), 1, .1f, 10);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    const std::array<Vertex3D, 4> vertices{{{{-1, -1, 0}, {0, 0, 1}, {0, 0}},
                                            {{1, -1, 0}, {0, 0, 1}, {1, 0}},
                                            {{1, 1, 0}, {0, 0, 1}, {1, 1}},
                                            {{-1, 1, 0}, {0, 0, 1}, {0, 1}}}};
    const std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D plane;
    Check(plane.Upload(&session.gpu, vertices, indices), "mapped material plane uploads");
    const auto folder = std::filesystem::temp_directory_path() /
                        ("LuxMaterialMaps-" + std::to_string(SDL_GetTicksNS()));
    std::filesystem::create_directories(folder);

    struct Cleanup {
        std::filesystem::path path;

        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{folder};

    const auto image = [&](const char* name, std::array<uint8, 4> pixel) {
        std::array<uint8, 22> bytes{};
        bytes[2] = 2;
        bytes[12] = bytes[14] = 1;
        bytes[16] = 32;
        bytes[17] = 40;
        bytes[18] = pixel[2];
        bytes[19] = pixel[1];
        bytes[20] = pixel[0];
        bytes[21] = pixel[3];
        const auto path = folder / name;
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto utf8 = path.u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    };
    const auto black = image("black.tga", {0, 0, 0, 255});
    const auto white = image("white.tga", {255, 255, 255, 255});
    const auto tilted = image("normal.tga", {255, 128, 128, 255});
    Render3DSettings settings;
    settings.mode = Render3DMode::Pbr;
    settings.shadows = settings.imageBasedLighting = settings.toneMapping = false;
    settings.environmentColor = {.2f, .2f, .2f};
    settings.sun.direction = {0, 0, -1};
    settings.sun.intensity = 2;
    const MaterialTextures3D* runtimeTextures = nullptr;
    const auto renderFrame = [&](Material3D material, const Mesh3D* geometry = nullptr) {
        GpuFrameCapture capture;
        Check(CaptureFrame(session, capture,
                           [&] {
                               session.gpu.Clear(0, 0, 0, 1);
                               Renderer3D::BeginScene(camera, settings, 1);
                               if (runtimeTextures)
                                   context.DrawMeshWithTextures(geometry ? *geometry : plane,
                                                                Mat4::Identity(), material,
                                                                *runtimeTextures);
                               else
                                   Renderer3D::DrawMesh(geometry ? *geometry : plane,
                                                        Mat4::Identity(), material);
                               Renderer3D::EndScene();
                               return true;
                           }),
              "mapped material frame captures");
        return capture;
    };
    const auto render = [&](Material3D material, const Mesh3D* geometry = nullptr) {
        return At(renderFrame(material, geometry), .5f, .5f);
    };
    const auto difference = [](auto a, auto b) {
        return std::abs(int(a[0]) - b[0]) + std::abs(int(a[1]) - b[1]) + std::abs(int(a[2]) - b[2]);
    };
    Material3D material;
    material.albedoColor = {.4f, .4f, .4f, 1};
    const auto base = render(material);
    material.normalTexture = tilted;
    const auto normalMapped = render(material);
    Check(difference(base, normalMapped) > 10, "normal texture changes actual lighting");
    const std::array<uint8, 4> normalPixel{255, 128, 128, 255};
    const std::array<uint8, 4> blackPixel{0, 0, 0, 255};
    GpuTexture runtimeNormal, runtimeRoughness;
    Check(runtimeNormal.Initialize(&session.gpu, 1, 1, normalPixel.data()) &&
              runtimeRoughness.Initialize(&session.gpu, 1, 1, blackPixel.data()),
          "runtime material maps upload");
    MaterialTextures3D generatedMaps;
    generatedMaps.normal = &runtimeNormal;
    runtimeTextures = &generatedMaps;
    material.normalTexture.clear();
    Check(render(material) == normalMapped, "runtime normal matches file-backed normal");
    runtimeTextures = nullptr;
    material.normalScale = 0;
    material.normalTexture.clear();
    material.metallic = 1;
    material.metallicTexture = black;
    const auto dielectric = render(material);
    material.metallicTexture = white;
    Check(difference(dielectric, render(material)) > 5,
          "metallic texture changes actual PBR response");
    material.metallic = 0;
    material.metallicTexture.clear();
    material.roughness = 1;
    material.roughnessTexture = black;
    const auto smooth = render(material);
    material.roughnessTexture = white;
    Check(difference(smooth, render(material)) > 5, "roughness texture changes actual highlight");
    material.roughnessTexture.clear();
    generatedMaps.normal = nullptr;
    generatedMaps.roughness = &runtimeRoughness;
    runtimeTextures = &generatedMaps;
    Check(render(material) == smooth, "runtime roughness matches file-backed roughness");
    runtimeTextures = nullptr;
    settings.sun.intensity = 0;
    material.occlusionTexture = black;
    const auto occluded = render(material);
    material.occlusionStrength = 0;
    Check(difference(occluded, render(material)) > 20,
          "AO texture modulates environment lighting with strength");
    auto repeatVertices = vertices;
    for (auto& vertex : repeatVertices)
        vertex.texCoords.x += .75f;
    Mesh3D repeatPlane;
    Check(repeatPlane.Upload(&session.gpu, repeatVertices, indices), "repeat plane uploads");
    std::array<uint8, 26> twoColour{};
    twoColour[2] = 2;
    twoColour[12] = 2;
    twoColour[14] = 1;
    twoColour[16] = 32;
    twoColour[17] = 40;
    twoColour[20] = twoColour[21] = twoColour[23] = twoColour[25] = 255;
    const auto repeatPath = folder / "repeat.tga";
    {
        std::ofstream output(repeatPath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(twoColour.data()), twoColour.size());
    }
    const auto repeatUtf8 = repeatPath.u8string();
    material = {};
    material.baseColorTexture =
        std::string(reinterpret_cast<const char*>(repeatUtf8.data()), repeatUtf8.size());
    material.textureSampling.anisotropic = false;
    material.textureSampling.addressMode = TextureAddressMode::Clamp;
    settings.mode = Render3DMode::Legacy;
    settings.sun.intensity = 1;
    const auto clamped = render(material, &repeatPlane);
    material.textureSampling.addressMode = TextureAddressMode::Repeat;
    const auto repeated = render(material, &repeatPlane);
    Check(clamped[1] > clamped[0] + 30 && repeated[0] > repeated[1] + 30,
          "Repeat sampler wraps UVs while Clamp retains the edge texel");
    material.textureSampling.anisotropic = true;
    Check(render(material, &repeatPlane)[0] > 20, "anisotropic Repeat sampler renders");

    constexpr size_t maskSize = 64;
    std::vector<uint8> mask(18 + maskSize * maskSize * 4, 255);
    std::fill_n(mask.begin(), 18, uint8{0});
    mask[2] = 2;
    mask[12] = mask[14] = maskSize;
    mask[16] = 32;
    mask[17] = 40;
    constexpr std::array<uint8, 4> alpha{192, 64, 0, 0};
    for (size_t i = 0; i < maskSize * maskSize; ++i)
        mask[18 + i * 4 + 3] = alpha[i % alpha.size()];
    const auto maskPath = folder / "coverage.tga";
    {
        std::ofstream output(maskPath, std::ios::binary);
        output.write(reinterpret_cast<const char*>(mask.data()), mask.size());
    }
    const auto maskUtf8 = maskPath.u8string();
    material.baseColorTexture = {reinterpret_cast<const char*>(maskUtf8.data()), maskUtf8.size()};
    auto minifiedVertices = vertices;
    for (auto& vertex : minifiedVertices)
        vertex.texCoords = vertex.texCoords * 16;
    Mesh3D minified;
    Check(minified.Upload(&session.gpu, minifiedVertices, indices), "minified plane uploads");
    for (const auto [cutoff, expected] : {std::pair{.5f, 64}, std::pair{.001f, 128}}) {
        material.alphaCutoff = cutoff;
        const auto capture = renderFrame(material, &minified);
        int covered = 0;
        for (uint32 y = capture.height / 2 - 8; y < capture.height / 2 + 8; ++y)
            for (uint32 x = capture.width / 2 - 8; x < capture.width / 2 + 8; ++x)
                covered += capture.rgba[(size_t(y) * capture.width + x) * 4] > 20;
        Check(std::abs(covered - expected) <= 4, "minified alpha mask retains authored coverage");
    }
    auto coloredVertices = vertices;
    for (auto& vertex : coloredVertices)
        vertex.color = {0, 1, 0, 1};
    Mesh3D coloredPlane;
    Check(coloredPlane.Upload(&session.gpu, coloredVertices, indices), "colored plane uploads");
    const auto green = render({}, &coloredPlane);
    Check(green[1] > green[0] + 30, "3D mesh upload preserves vertex color");
    for (auto& vertex : coloredVertices)
        vertex.color = {1, 0, 0, 1};
    Check(coloredPlane.UpdateVertices(coloredVertices), "colored plane updates");
    const auto red = render({}, &coloredPlane);
    Check(red[0] > red[1] + 30, "3D mesh update preserves vertex color");
    Check(context.GetLastAssetError().empty(), "material texture loading has no hidden errors");
}

void SceneSubmission() {
    std::printf("[scene_submission]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    Session session(192, 192, config, true);
    if (!session.ok)
        return Check(false, "session for repeated scene submission");

    const std::array<Vertex3D, 4> quad{{{{-.5f, -.5f, 0}, {0, 0, 1}, {0, 0}},
                                        {{.5f, -.5f, 0}, {0, 0, 1}, {1, 0}},
                                        {{.5f, .5f, 0}, {0, 0, 1}, {1, 1}},
                                        {{-.5f, .5f, 0}, {0, 0, 1}, {0, 1}}}};
    const std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D mesh;
    const std::array<uint8, 4> red{255, 0, 0, 255}, green{0, 255, 0, 255};
    GpuTexture redMap, greenMap;
    if (!mesh.Upload(&session.gpu, quad, indices) ||
        !redMap.Initialize(&session.gpu, 1, 1, red.data()) ||
        !greenMap.Initialize(&session.gpu, 1, 1, green.data()))
        return Check(false, "repeated scene mesh and textures upload");

    Renderer3D::Context context(&session.gpu);
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadowResolution = 1024;
    settings.sun.direction = {0, 0, -1};
    settings.sun.intensity = 0;
    PerspectiveCamera camera(ToRadians(60), 1, .1f, 100);
    camera.SetPosition({0, 0, 8});

    constexpr uint32 drawCount = 128;
    const auto render = [&](bool grouped, GpuFrameCapture& capture) {
        return CaptureFrame(session, capture, [&] {
            if (!session.gpu.Clear(0, 0, 0, 1))
                return false;
            context.BeginScene(camera, settings, 1);
            for (uint32 i = 0; i < drawCount; ++i) {
                const uint32 index = grouped ? (i % 64) * 2 + i / 64 : i;
                const auto position = Vec3{(static_cast<int>(index % 16) - 7.5) * .4,
                                           (static_cast<int>(index / 16) - 3.5) * .4, 0};
                context.DrawMesh(mesh, Mat4::Translate(position) * Mat4::Scale({.32, .32, .32}),
                                 Material3D{}, index % 2 == 0 ? &redMap : &greenMap);
            }
            context.EndScene();
            return context.GetLastAssetError().empty();
        });
    };

    GpuFrameCapture interleaved, grouped;
    if (!render(false, interleaved))
        return Check(false, "interleaved material submission renders");
    const auto before = session.gpu.GetFrameStats();
    if (!render(true, grouped))
        return Check(false, "grouped material submission renders");
    const auto after = session.gpu.GetFrameStats();
    uint32 redPixels = 0, greenPixels = 0;
    for (size_t i = 0; i < grouped.rgba.size(); i += 4) {
        redPixels += grouped.rgba[i] > 64 && grouped.rgba[i + 1] == 0;
        greenPixels += grouped.rgba[i + 1] > 64 && grouped.rgba[i] == 0;
    }
    std::printf("  %u indexed scene draws across two passes, pipeline binds=%u, "
                "descriptor binds interleaved=%u grouped=%u\n",
                2 * drawCount, after.pipelineBinds, before.descriptorBinds, after.descriptorBinds);
    Check(grouped.rgba == interleaved.rgba && redPixels > 100 && greenPixels > 100,
          "reusing material sets retains geometry and both base-color textures");
    Check(before.drawCalls == 2 * drawCount + 1 && after.drawCalls == before.drawCalls,
          "reusing state retains every shadow and HDR submission");
    Check(before.pipelineBinds == 3 && after.pipelineBinds == 3,
          "the shadow, HDR and tone passes each bind one pipeline");
    Check(before.descriptorBinds == 4 * drawCount + 4 && after.descriptorBinds == 2 * drawCount + 8,
          "a material run only binds its shared texture descriptors once per pass");
    Check(session.gpu.GetCounters().validationErrors == 0,
          "reusing draw state creates no GPU validation errors");

    settings.shadows = false;
    settings.environmentColor = {1, 1, 1};
    camera.SetPosition({0, 0, 3});
    std::array<Vertex3D, 3> vertices{quad[0], quad[1], quad[2]};
    const std::array<uint32, 3> triangle{0, 1, 2};
    Mesh3D dynamic;
    Check(dynamic.Upload(&session.gpu, vertices, triangle), "dynamic triangle uploaded");
    vertices[0].position.x = 0;
    for (const bool editAfterDraw : {false, true, false}) {
        GpuFrameCapture capture;
        Check(CaptureFrame(session, capture,
                           [&] {
                               session.gpu.Clear(0, 0, 0, 1);
                               context.BeginScene(camera, settings, 1);
                               context.DrawMesh(dynamic, Mat4{}, {}, &redMap);
                               const bool updated =
                                   !editAfterDraw || dynamic.UpdateVertices(vertices);
                               context.EndScene();
                               return updated;
                           }),
              "dynamic triangle frame captured");
        Check(At(capture, .59f, .62f)[0] > 100,
              "dynamic mesh vertices and the queued draw use the same local origin");
    }
    for (auto& vertex : vertices)
        vertex.position.x += 1e12;
    GpuFrameCapture oldLocation, newLocation;
    Check(CaptureFrame(session, oldLocation,
                       [&] {
                           session.gpu.Clear(0, 0, 0, 1);
                           context.BeginScene(camera, settings, 1);
                           context.DrawMesh(dynamic, Mat4{}, {}, &redMap);
                           context.EndScene();
                           return dynamic.UpdateVertices(vertices);
                       }) &&
              At(oldLocation, .59f, .62f)[0] > 100,
          "recorded geometry remains intact when an update rebases the mesh before presentation");
    camera.SetPosition({1e12, 0, 3});
    camera.SetTarget({1e12, 0, 0});
    Check(CaptureFrame(session, newLocation,
                       [&] {
                           session.gpu.Clear(0, 0, 0, 1);
                           context.BeginScene(camera, settings, 1);
                           context.DrawMesh(dynamic, Mat4{}, {}, &redMap);
                           context.EndScene();
                           return true;
                       }) &&
              At(newLocation, .59f, .62f)[0] > 100,
          "rebased vertices keep their precision at a distant camera");
}

void WaterComposite() {
    std::printf("[water_composite]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    Session session(192, 192, config, true);
    if (!session.ok)
        return Check(false, "session for water compositing");

    const std::array<Vertex3D, 4> quad{{{{-.5, -.5, 0}, {0, 0, 1}, {0, 0}},
                                        {{.5, -.5, 0}, {0, 0, 1}, {1, 0}},
                                        {{.5, .5, 0}, {0, 0, 1}, {1, 1}},
                                        {{-.5, .5, 0}, {0, 0, 1}, {0, 1}}}};
    const std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D mesh, crest, sloped;
    auto crestVertices = quad;
    auto slopedVertices = quad;
    for (auto& vertex : crestVertices)
        vertex.color.w = 0;
    for (auto& vertex : slopedVertices)
        vertex.normal = {.8, 0, .6};
    if (!mesh.Upload(&session.gpu, quad, indices) ||
        !crest.Upload(&session.gpu, crestVertices, indices) ||
        !sloped.Upload(&session.gpu, slopedVertices, indices))
        return Check(false, "water fixture meshes upload");

    Renderer3D::Context context(&session.gpu);
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadows = false;
    settings.toneMapping = false;
    settings.environmentColor = {.5, .5, .5};
    settings.sun.intensity = 0;
    PerspectiveCamera camera(ToRadians(60), 1, .1f, 100);
    camera.SetPosition({0, 0, 3});

    const auto draw = [&](const Mesh3D* surface, Vec3 absorption, double floorDepth,
                          Vec4 floorColor, bool foreground, bool stripes) {
        GpuFrameCapture result;
        if (!CaptureFrame(session, result, [&] {
                if (!session.gpu.Clear(0, 0, 0, 1))
                    return false;
                context.BeginScene(camera, settings, 1);
                Material3D floor;
                floor.albedoColor = floorColor;
                if (stripes) {
                    floor.albedoColor = {1, 0, 0, 1};
                    context.DrawMesh(mesh,
                                     Mat4::Translate({-3, 0, -floorDepth}) * Mat4::Scale({6, 6, 1}),
                                     floor);
                    floor.albedoColor = {0, 1, 0, 1};
                    context.DrawMesh(
                        mesh, Mat4::Translate({3, 0, -floorDepth}) * Mat4::Scale({6, 6, 1}), floor);
                } else {
                    context.DrawMesh(
                        mesh, Mat4::Translate({0, 0, -floorDepth}) * Mat4::Scale({6, 6, 1}), floor);
                }
                if (foreground) {
                    Material3D marker;
                    marker.albedoColor = {1, 0, 0, 1};
                    context.DrawMesh(mesh, Mat4::Translate({0, 0, .5}) * Mat4::Scale({.5, .5, 1}),
                                     marker);
                }
                if (surface) {
                    Material3D water;
                    water.albedoColor = {0, 0, 0, 1};
                    water.roughness = .06f;
                    water.water = WaterMaterial3D{absorption, 1.333f};
                    context.DrawMesh(*surface, Mat4::Scale({2, 2, 1}), water);
                }
                context.EndScene();
                return context.GetLastAssetError().empty();
            })) {
            std::printf("  water frame failed: %s / %s\n", context.GetLastAssetError().c_str(),
                        session.gpu.GetLastError().c_str());
            Check(false, "water frame captures");
            return GpuFrameCapture{};
        }
        return result;
    };
    const auto read = [](const GpuFrameCapture& frame, float x = .5f) {
        return frame.rgba.empty() ? std::array<int, 4>{} : At(frame, x, .5f);
    };
    const auto clearWater = read(draw(&mesh, {}, 1.5, {1, 1, 1, 1}, false, false));
    const auto absorbed = read(draw(&mesh, {1.2, 0, 0}, 1.5, {1, 1, 1, 1}, false, false));
    const auto deep = read(draw(&mesh, {}, 1.5, {0, 0, 0, 1}, false, false));
    const auto shore = read(draw(&mesh, {}, .07, {0, 0, 0, 1}, false, false));
    const auto crestFoam = read(draw(&crest, {}, 1.5, {0, 0, 0, 1}, false, false));
    const auto marker = read(draw(nullptr, {}, 1.5, {1, 1, 1, 1}, true, false));
    const auto markerOverWater = read(draw(&mesh, {1.2, 0, 0}, 1.5, {1, 1, 1, 1}, true, false));
    const auto unbent = draw(&mesh, {}, 1.5, {}, false, true);
    const auto refracted = draw(&sloped, {}, 1.5, {}, false, true);
    const auto baseSide = read(unbent, .505f);
    const auto bentSide = read(refracted, .505f);
    std::printf("  water center clear=%d,%d,%d absorbed=%d,%d,%d deep=%d shore=%d crest=%d\n",
                clearWater[0], clearWater[1], clearWater[2], absorbed[0], absorbed[1], absorbed[2],
                deep[0], shore[0], crestFoam[0]);
    std::printf("  water boundary unbent=%d,%d refracted=%d,%d\n", baseSide[0], baseSide[1],
                bentSide[0], bentSide[1]);
    Check(absorbed[0] + 60 < clearWater[0] && std::abs(absorbed[1] - clearWater[1]) < 4,
          "water uses scene depth and per-channel Beer-Lambert absorption");
    Check(shore[0] > deep[0] + 45 && crestFoam[0] > deep[0] + 45,
          "shore and crest foam brighten water independently");
    Check(markerOverWater == marker && marker[0] > 150 && marker[1] == 0,
          "opaque geometry in front of water retains its color and depth");
    Check(baseSide[1] > baseSide[0] + 40 && bentSide[0] > bentSide[1] + 30,
          "slope refracts the opaque scene across a color boundary");

    const auto backgroundRed = Vec4{1, 0, 0, 1};
    const auto backgroundBlue = Vec4{0, 0, 1, 1};
    camera.SetPosition({0, 0, -1});
    const auto transmittedRed = read(draw(&mesh, {}, -1, backgroundRed, false, false));
    const auto transmittedBlue = read(draw(&mesh, {}, -1, backgroundBlue, false, false));
    Check(transmittedRed[0] > transmittedRed[2] + 40 &&
              transmittedBlue[2] > transmittedBlue[0] + 40,
          "an underwater ray below the critical angle transmits the background color");
    camera.SetPosition({2.5, 0, -1});
    const auto reflectedRed = read(draw(&mesh, {}, -1, backgroundRed, false, false));
    const auto reflectedBlue = read(draw(&mesh, {}, -1, backgroundBlue, false, false));
    Check(reflectedRed == reflectedBlue && reflectedRed[0] > 100 &&
              reflectedRed[0] == reflectedRed[2],
          "total internal reflection excludes the background without ray queries");
    camera.SetPosition({0, 0, 3});

    settings.environmentColor = {0, 0, 0};
    settings.sun = {{0, 0, -1}, {1, 1, 1}, .5f};
    const auto glint = [&](float indexOfRefraction) {
        GpuFrameCapture frame;
        if (!CaptureFrame(session, frame, [&] {
                if (!session.gpu.Clear(0, 0, 0, 1))
                    return false;
                context.BeginScene(camera, settings, 1);
                Material3D water = WaterSurface::MakeMaterial();
                water.albedoColor = {0, 0, 0, 1};
                water.roughness = .3f;
                water.water->indexOfRefraction = indexOfRefraction;
                context.DrawMesh(mesh, Mat4::Scale({2, 2, 1}), water);
                context.EndScene();
                return context.GetLastAssetError().empty();
            })) {
            Check(false, "water glint frame captures");
            return 0;
        }
        return read(frame)[0];
    };
    const int lowIorGlint = glint(1.1f);
    const int waterGlint = glint(1.333f);
    Check(lowIorGlint > 0 && waterGlint > lowIorGlint + 20,
          "directional glint uses the water material's index of refraction");
    Check(session.gpu.GetCounters().validationErrors == 0,
          "the opaque color and depth snapshots introduce no GPU validation errors");
}

void FroxelFog() {
    Session session(128, 128, {}, true);
    if (!session.ok)
        return Check(false, "session for froxel fog");
    const std::array<Vertex3D, 4> quad{{{{-.5, -.5, 0}, {0, 0, 1}, {0, 0}},
                                        {{.5, -.5, 0}, {0, 0, 1}, {1, 0}},
                                        {{.5, .5, 0}, {0, 0, 1}, {1, 1}},
                                        {{-.5, .5, 0}, {0, 0, 1}, {0, 1}}}};
    const std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D plane;
    if (!plane.Upload(&session.gpu, quad, indices))
        return Check(false, "froxel fixture plane uploads");

    Renderer3D::Context context(&session.gpu);
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadows = false;
    settings.toneMapping = false;
    settings.environmentColor = {.5, .5, .5};
    settings.sun.intensity = 0;
    settings.fog.color = {};
    settings.fog.heightFalloff = 0;
    settings.fog.maxDistance = 24;
    PerspectiveCamera camera(ToRadians(60), 1, .1, 100);
    camera.SetTarget({0, 0, -1});
    camera.SetPosition({0, 0, 0});
    Material3D wall;
    wall.albedoColor = {.8f, .8f, .8f, 1};
    wall.alphaMode = MaterialAlphaMode::Opaque;
    const auto render = [&](const Render3DSettings& options, double depth, bool water, bool blocker,
                            std::array<int, 4>& center) {
        GpuFrameCapture frame;
        if (!CaptureFrame(session, frame, [&] {
                if (!session.gpu.Clear(0, 0, 0, 1))
                    return false;
                context.BeginScene(camera, options, 1);
                context.DrawMesh(plane, Mat4::Translate({0, 0, -depth}) * Mat4::Scale({24, 24, 1}),
                                 wall);
                if (water) {
                    auto surface = WaterSurface::MakeMaterial();
                    surface.water->absorption = {};
                    context.DrawMesh(plane, Mat4::Translate({0, 0, -3}) * Mat4::Scale({5, 5, 1}),
                                     surface);
                }
                if (blocker)
                    context.DrawCube({0, 2, -7}, {10, .2, 13}, wall);
                context.EndScene();
                return context.GetLastAssetError().empty();
            })) {
            std::printf("  froxel frame failed: %s / %s\n", context.GetLastAssetError().c_str(),
                        session.gpu.GetLastError().c_str());
            return false;
        }
        center = At(frame, .5f, .5f);
        return true;
    };

    constexpr float density = .15f;
    auto fog = settings;
    fog.fog.density = density;
    std::array<int, 4> baseline, near, far, waterBaseline, waterFog;
    if (!render(settings, 12, false, false, baseline))
        return Check(false, "fog-disabled reference frame captures");
    Check(session.gpu.GetFrameStats().volumetricFogPasses == 0, "zero density leaves fog disabled");
    if (!render(fog, 3, false, false, near) || !render(fog, 12, false, false, far) ||
        !render(settings, 12, true, false, waterBaseline) ||
        !render(fog, 12, true, false, waterFog))
        return Check(false, "froxel frames capture at different scene and water depths");
    const auto attenuated = [density](int sample, double depth) {
        const double srgb = sample / 255.;
        const double linear = srgb <= .04045 ? srgb / 12.92 : std::pow((srgb + .055) / 1.055, 2.4);
        const double absorbed = linear * std::exp(-density * depth);
        return static_cast<int>(
            std::lround(255 * (absorbed <= .0031308 ? absorbed * 12.92
                                                    : 1.055 * std::pow(absorbed, 1 / 2.4) - .055)));
    };
    std::printf("  froxel base=%d near=%d far=%d water=%d->%d\n", baseline[0], near[0], far[0],
                waterBaseline[0], waterFog[0]);
    Check(baseline[0] > 100 && far[0] + 30 < near[0] &&
              std::abs(near[0] - attenuated(baseline[0], 3)) <= 3 &&
              std::abs(far[0] - attenuated(baseline[0], 12)) <= 3,
          "the froxel transmittance follows Beer-Lambert over metric scene depth");
    Check(waterBaseline[0] > 45 && std::abs(waterFog[0] - attenuated(waterBaseline[0], 3)) <= 3,
          "fog integrates to the composited water depth");
    const auto counters = session.gpu.GetFrameStats();
    Check(counters.volumetricFogPasses == 1 && counters.computeDispatches == 1,
          "the scene dispatches one froxel integration pass");

    auto sun = fog;
    sun.fog.color = {1, 1, 1};
    sun.fog.anisotropy = 0;
    sun.sun.direction = {0, -1, 0};
    sun.sun.intensity = 7;
    sun.environmentColor = {};
    std::array<int, 4> unshadowed, shadowed;
    if (!render(sun, 12, false, true, unshadowed))
        return Check(false, "unshadowed froxel scattering captures");
    sun.fog.rayTracedShadows = true;
    if (!render(sun, 12, false, true, shadowed))
        return Check(false, "ray-traced froxel scattering captures");
    if (session.gpu.GetInfo().rayTracing) {
        std::printf("  froxel sunlight unoccluded=%d occluded=%d\n", unshadowed[0], shadowed[0]);
        Check(session.gpu.GetFrameStats().rayVolumetricFogPasses == 1 &&
                  shadowed[0] + 20 < unshadowed[0],
              "hardware visibility occludes the sun inside the fog volume");
    } else {
        Check(session.gpu.GetFrameStats().rayVolumetricFogPasses == 0 && shadowed == unshadowed,
              "fog retains its raster fallback without hardware ray queries");
    }
    Check(session.gpu.GetCounters().validationErrors == 0, "froxel barriers validate");
}

void OceanFft() {
    Session session(144, 144, {}, true);
    if (!session.ok)
        return Check(false, "session for GPU ocean");

    OceanSpectrumSettings spectrumSettings;
    constexpr uint32 n = 16, stride = n + 1;
    spectrumSettings.resolution = n;
    spectrumSettings.patchLength = 20;
    spectrumSettings.spectrumAmplitude = .025;
    spectrumSettings.choppiness = 1.6;
    spectrumSettings.foamThreshold = .97;
    OceanSpectrum spectrum;
    Mesh3D gpuMesh, cpuMesh;
    if (!spectrum.Initialize(spectrumSettings) || !gpuMesh.UploadOcean(&session.gpu, spectrum))
        return Check(false, "GPU ocean spectrum upload");
    Check(!gpuMesh.UpdateOcean(0), "GPU ocean requires an open frame");

    std::vector<uint32> indices;
    for (uint32 z = 0; z < n; ++z)
        for (uint32 x = 0; x < n; ++x) {
            const uint32 a = z * stride + x, b = a + stride;
            indices.insert(indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    std::array<Vertex3D, stride * stride> vertices;
    const auto sampleCpu = [&](float time) {
        if (!spectrum.Update(time))
            return false;
        const auto grid = spectrum.Grid();
        for (uint32 z = 0; z <= n; ++z)
            for (uint32 x = 0; x <= n; ++x) {
                const auto& sample = grid[(z % n) * n + (x % n)];
                const double u = static_cast<double>(x) / n;
                const double v = static_cast<double>(z) / n;
                const Vec3 base{(u - .5) * spectrumSettings.patchLength, 0,
                                (v - .5) * spectrumSettings.patchLength};
                vertices[z * stride + x] = {base + sample.displacement,
                                            sample.normal,
                                            {u, v},
                                            {1, 1, 1, static_cast<float>(1 - sample.foam)}};
            }
        return cpuMesh.Upload(&session.gpu, vertices, indices);
    };

    Renderer3D::Context context(&session.gpu);
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadows = false;
    settings.environmentColor = {.35, .35, .35};
    settings.sun.direction = {-.5, -1, -.25};
    PerspectiveCamera camera(ToRadians(55), 1, .1, 80);
    camera.SetPosition({8, 10, 13});
    camera.SetTarget({0, 0, 0});
    const auto material = WaterSurface::MakeMaterial();
    const auto capture = [&](GpuFrameCapture& frame, Mesh3D& mesh, float time, bool compute) {
        return CaptureFrame(session, frame, [&] {
            if ((compute && !mesh.UpdateOcean(time)) || !session.gpu.Clear(.04f, .05f, .07f, 1))
                return false;
            context.BeginScene(camera, settings, 1);
            Material3D bottom;
            bottom.albedoColor = {.1f, .18f, .28f, 1};
            context.DrawCube({0, -3, 0}, {30, 1, 30}, bottom);
            context.DrawMesh(mesh, Mat4::Identity(), material);
            context.EndScene();
            return context.GetStats().meshCount == 2;
        });
    };

    const auto differentPixels = [](const GpuFrameCapture& a, const GpuFrameCapture& b) {
        size_t different = 0;
        for (size_t pixel = 0; pixel < a.rgba.size(); pixel += 4)
            if (std::abs(a.rgba[pixel] - b.rgba[pixel]) > 3 ||
                std::abs(a.rgba[pixel + 1] - b.rgba[pixel + 1]) > 3 ||
                std::abs(a.rgba[pixel + 2] - b.rgba[pixel + 2]) > 3)
                ++different;
        return different;
    };
    GpuFrameCapture previousGpu;
    for (float time : {.375f, 1.75f}) {
        GpuFrameCapture cpu, gpu;
        if (!sampleCpu(time) || !capture(cpu, cpuMesh, time, false) ||
            !capture(gpu, gpuMesh, time, true) || cpu.rgba.size() != gpu.rgba.size() ||
            cpu.rgba.empty() ||
            (!previousGpu.rgba.empty() && previousGpu.rgba.size() != gpu.rgba.size())) {
            std::printf("  %s / %s\n", session.gpu.GetLastError().c_str(),
                        context.GetLastAssetError().c_str());
            return Check(false, "CPU and GPU ocean frames render and capture");
        }
        Check(session.gpu.GetFrameStats().computeDispatches == 3,
              "GPU dispatches both FFT axes and vertex assembly");
        const auto pixels = gpu.rgba.size() / 4;
        const auto differences = differentPixels(cpu, gpu);
        std::printf("  FFT %.3f: %zu of %zu pixels differ by more than 3 levels\n", time,
                    differences, pixels);
        Check(differences * 100 < pixels, "GPU displacement, normals and foam match CPU render");
        if (!previousGpu.rgba.empty())
            Check(differentPixels(previousGpu, gpu) * 20 > pixels,
                  "GPU ocean changes between simulation times");
        previousGpu = std::move(gpu);
    }
    Check(session.gpu.GetCounters().validationErrors == 0, "GPU ocean barriers validate");
}

void RayTracedLighting() {
    std::printf("[ray_traced_lighting]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    Session session(128, 128, config, true);
    if (!session.ok)
        return Check(false, "session for hardware ray tracing");

    constexpr std::array<Vec3, 4> horizontal{
        {{-.5, -.5, 0}, {.5, -.5, 0}, {.5, .5, 0}, {-.5, .5, 0}}};
    constexpr std::array<Vec3, 4> vertical{
        {{0, -.5, -.5}, {0, .5, -.5}, {0, .5, .5}, {0, -.5, .5}}};
    constexpr std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    const auto quad = [&](Mesh3D& mesh, const std::array<Vec3, 4>& positions, Vec3 normal) {
        std::array<Vertex3D, 4> vertices;
        for (size_t i = 0; i < vertices.size(); ++i)
            vertices[i] = {positions[i], normal, {}};
        return mesh.Upload(&session.gpu, vertices, indices);
    };
    Mesh3D screenPlane;
    if (!quad(screenPlane, horizontal, {0, 0, 1}))
        return Check(false, "ray tracing fixture geometry uploads");

    Renderer3D::Context context(&session.gpu);
    PerspectiveCamera camera(ToRadians(35), 1, .1, 50);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadows = false;
    settings.toneMapping = false;
    settings.environmentColor = {.12, .12, .12};
    settings.sun.direction = {-2, 0, -1};
    settings.sun.intensity = 3;
    const auto capture = [&](GpuFrameCapture& frame, auto&& draw) {
        return CaptureFrame(session, frame, [&] {
            if (!session.gpu.Clear(0, 0, 0, 1))
                return false;
            context.BeginScene(camera, settings, 1);
            draw();
            context.EndScene();
            return context.GetLastAssetError().empty();
        });
    };
    Material3D matte;
    matte.alphaMode = MaterialAlphaMode::Opaque;
    matte.albedoColor = {.9f, .9f, .9f, 1};
    const auto shadowScene = [&] {
        context.DrawMesh(screenPlane, Mat4::Scale({3, 3, 1}), matte);
        // The center's ray toward the sun intersects this quad at (2, 0, 1),
        // outside the camera frustum. The raster shadow pass is disabled.
        const Mat4 caster = Mat4::Translate({2, 0, 1}) * Mat4::Scale({.55, .55, 1});
        context.DrawMesh(screenPlane, caster, matte);
    };
    GpuFrameCapture unshadowed, shadowed;
    if (!capture(unshadowed, shadowScene))
        return Check(false, "raster receiver frame captures");
    settings.rayTracing.shadows = true;
    if (!capture(shadowed, shadowScene)) {
        std::printf("  graphics error: %s / %s\n", context.GetLastAssetError().c_str(),
                    session.gpu.GetLastError().c_str());
        return Check(false, "ray tracing receiver frame captures");
    }
    const auto light = At(unshadowed, .5f, .5f);
    const auto shade = At(shadowed, .5f, .5f);
    const auto rayStats = session.gpu.GetFrameStats();
    std::printf("  shadow pixel %d -> %d, RT passes %u, AS builds %u\n", light[0], shade[0],
                rayStats.rayTracedPasses, rayStats.accelerationStructureBuilds);
    if (!session.gpu.GetInfo().rayTracing) {
        Check(light == shade && rayStats.rayTracedPasses == 0 &&
                  rayStats.accelerationStructureBuilds == 0,
              "ray tracing settings preserve raster output without ray query support");
        return;
    }
    Check(rayStats.rayTracedPasses > 0 && rayStats.accelerationStructureBuilds >= 2,
          "builds a BLAS and TLAS and submits the ray-traced scene pass");
    Check(light[0] > shade[0] + 25, "an off-screen caster blocks sunlight through the TLAS");

    Mesh3D firstMirror, secondMirror, tiltedWater;
    if (!quad(firstMirror, horizontal, {.555570233, 0, .831469612}) ||
        !quad(secondMirror, vertical, {1, 0, 0}) ||
        !quad(tiltedWater, horizontal, {.906307787, 0, .422618262}))
        return Check(false, "reflection and refraction fixture geometry uploads");

    settings.sun.intensity = 0;
    settings.rayTracing = {};
    settings.rayTracing.reflections = true;
    settings.rayTracing.maxBounces = 1;
    Material3D mirror;
    mirror.alphaMode = MaterialAlphaMode::Opaque;
    mirror.albedoColor = {1, 1, 1, 1};
    mirror.metallic = 1;
    mirror.roughness = .045f;
    Material3D red;
    red.alphaMode = MaterialAlphaMode::Opaque;
    red.albedoColor = {1, 0, 0, 1};
    const auto reflectionScene = [&] {
        context.DrawMesh(firstMirror, Mat4::Scale({2, 2, 2}), mirror);
        context.DrawMesh(secondMirror, Mat4::Translate({3, 0, 1.24}), mirror);
        // From the first mirror, the center ray reaches x=3, z=1.24 and
        // bounces back toward this red plane at x=1.55, z=1.84.
        context.DrawMesh(secondMirror, Mat4::Translate({1.55, 0, 1.84}) * Mat4::Scale({1, .8, .45}),
                         red);
    };
    GpuFrameCapture oneBounce, twoBounces;
    if (!capture(oneBounce, reflectionScene))
        return Check(false, "single-bounce reflection captures");
    settings.rayTracing.maxBounces = 2;
    if (!capture(twoBounces, reflectionScene))
        return Check(false, "multi-bounce reflection captures");
    const auto once = At(oneBounce, .5f, .5f);
    const auto twice = At(twoBounces, .5f, .5f);
    std::printf("  reflection one=%d,%d,%d two=%d,%d,%d\n", once[0], once[1], once[2], twice[0],
                twice[1], twice[2]);
    Check(std::abs(once[0] - once[1]) < 10 && twice[0] > twice[1] + 25,
          "the second specular bounce reaches the off-screen red plane");

    settings.rayTracing = {};
    Material3D water;
    water.albedoColor = {0, 0, 0, 1};
    water.roughness = .06f;
    water.water = WaterMaterial3D{{}, 2};
    const auto refractionScene = [&] {
        context.DrawMesh(tiltedWater, Mat4::Scale({2, 2, 2}), water);
        context.DrawMesh(screenPlane, Mat4::Translate({-3.14, 0, -4}), red);
    };
    GpuFrameCapture rasterWater, tracedWater;
    if (!capture(rasterWater, refractionScene))
        return Check(false, "raster water frame captures");
    settings.rayTracing.refractions = true;
    if (!capture(tracedWater, refractionScene))
        return Check(false, "ray-traced water frame captures");
    const auto rasterColor = At(rasterWater, .5f, .5f);
    const auto tracedColor = At(tracedWater, .5f, .5f);
    std::printf("  refraction raster=%d,%d,%d RT=%d,%d,%d\n", rasterColor[0], rasterColor[1],
                rasterColor[2], tracedColor[0], tracedColor[1], tracedColor[2]);
    Check(std::abs(rasterColor[0] - rasterColor[1]) < 10 && tracedColor[0] > tracedColor[1] + 25,
          "ray-traced water refraction sees geometry absent from opaque scene color");
    Check(session.gpu.GetCounters().validationErrors == 0,
          "ray queries and acceleration structure barriers validate");
}

void VirtualShadowMap() {
    std::printf("[virtual_shadow_map]\n");
    Session session(160, 160, {}, true);
    if (!session.ok)
        return Check(false, "session for virtual shadows");

    Renderer3D::Context context(&session.gpu);
    PerspectiveCamera camera(ToRadians(55), 1, .1, 100);
    camera.SetPosition({6, 7, 9});
    camera.SetTarget({0, 1, 0});
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.environmentColor = {.04, .04, .04};
    settings.sun.direction = {.7, -1, .3};
    settings.sun.intensity = 3;
    settings.shadowResolution = 1024;
    Material3D ground, object;
    ground.albedoColor = {.5f, .5f, .5f, 1};
    ground.alphaMode = MaterialAlphaMode::Opaque;
    object.albedoColor = {.6f, .15f, .04f, 1};

    const auto capture = [&](GpuFrameCapture& frame, double receiverWidth = 10.,
                             double receiverDepth = 0., double receiverZ = 0.) {
        return CaptureFrame(session, frame, [&] {
            if (!session.gpu.Clear(.03f, .04f, .05f, 1))
                return false;
            context.BeginScene(camera, settings, 1);
            context.DrawCube({0, -.1, receiverZ},
                             {receiverWidth, .2, receiverDepth > 0 ? receiverDepth : receiverWidth},
                             ground);
            context.DrawCube({0, 1, 0}, {1, 2, 1}, object);
            context.EndScene();
            return context.GetLastAssetError().empty();
        });
    };

    GpuFrameCapture lit, paged, dense;
    settings.shadows = false;
    if (!capture(lit))
        return Check(false, "unshadowed receiver renders");
    const auto baselineBytes = session.gpu.GetMemoryStats().allocationBytes;

    settings.shadows = true;
    settings.virtualShadows = true;
    if (!capture(paged) || lit.rgba.size() != paged.rgba.size())
        return Check(false, "virtual shadow pages render");
    const auto pages = session.gpu.GetFrameStats().virtualShadowPages;
    const auto pagedBytes = session.gpu.GetMemoryStats().allocationBytes - baselineBytes;
    Check(pages > 1 && pages <= 9 && pagedBytes == 768ull * 768 * 4,
          "receiver coverage maps multiple pages into the smaller physical atlas");
    int darkened = 0;
    for (size_t i = 0; i < paged.rgba.size(); i += 4)
        darkened += lit.rgba[i] > paged.rgba[i] + 20;
    Check(darkened > 25, "casters darken receivers through the page table");

    settings.virtualShadows = false;
    if (!capture(dense) || dense.rgba.size() != paged.rgba.size())
        return Check(false, "dense reference shadows render");
    int changed = 0;
    for (size_t i = 0; i < dense.rgba.size(); i += 4)
        changed += std::abs(int(dense.rgba[i]) - int(paged.rgba[i])) > 2;
    std::printf("  resident pages %u, atlas bytes %llu, changed pixels %d\n", pages,
                static_cast<unsigned long long>(pagedBytes), changed);
    Check(changed < 16, "paged PCF matches dense shadow edges across virtual page boundaries");

    settings.virtualShadows = true;
    Check(capture(dense, 120.) && session.gpu.GetFrameStats().virtualShadowPages == 0,
          "oversubscribed atlas falls back to dense coverage");
    Check(session.gpu.GetFrameStats().requestedShadowPages == 16,
          "dense fallback reports the requested virtual pages");

    settings.sun.direction = {0, -1, 0};
    GpuFrameCapture rectangular;
    const bool rectangleCaptured = capture(rectangular, 40., 24., -9.);
    const auto rectangleStats = session.gpu.GetFrameStats();
    Check(rectangleCaptured && rectangleStats.requestedShadowPages == 12 &&
              rectangleStats.virtualShadowPages == 12,
          "a rectangular atlas keeps twelve requested pages resident");
    settings.virtualShadows = false;
    if (!capture(dense, 40., 24., -9.) || rectangular.rgba.size() != dense.rgba.size())
        return Check(false, "rectangular and dense reference frames capture");
    changed = 0;
    for (size_t i = 0; i < dense.rgba.size(); i += 4)
        changed += std::abs(int(dense.rgba[i]) - int(rectangular.rgba[i])) > 2;
    Check(changed < 16, "rectangular atlas preserves dense shadow coverage");
    Check(session.gpu.GetCounters().validationErrors == 0,
          "virtual shadow pass reports no GPU validation errors");
}

void RadianceCascades() {
    std::printf("[radiance_cascades]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    Session session(128, 128, config, true);
    if (!session.ok)
        return Check(false, "session for radiance cascades");

    Renderer3D::Context context(&session.gpu);
    PerspectiveCamera camera(ToRadians(35), 1, .1, 50);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadows = false;
    settings.toneMapping = false;
    settings.environmentColor = {0, 0, 0};
    settings.sun.direction = {1, 0, 0};
    settings.sun.color = {1, 1, 1};
    settings.sun.intensity = 9;
    settings.radianceCascades.spacing = 2;
    Material3D receiver;
    receiver.alphaMode = MaterialAlphaMode::Opaque;
    Material3D red;
    red.alphaMode = MaterialAlphaMode::Opaque;
    red.albedoColor = {1, 0, 0, 1};
    const auto capture = [&](GpuFrameCapture& frame) {
        return CaptureFrame(session, frame, [&] {
            if (!session.gpu.Clear(0, 0, 0, 1))
                return false;
            context.BeginScene(camera, settings, 1);
            context.DrawCube({0, 0, -.1}, {3, 3, .2}, receiver);
            // The sun illuminates this red wall from the left. It lies outside the
            // camera frustum and beyond the nearest interval around the center probes.
            context.DrawCube({8, 0, 4}, {.2, 16, 16}, red);
            context.EndScene();
            return context.GetLastAssetError().empty();
        });
    };

    GpuFrameCapture baseline, withCascades;
    if (!capture(baseline))
        return Check(false, "radiance baseline captures");
    settings.radianceCascades.strength = 1;
    if (!capture(withCascades))
        return Check(false, "radiance cascades frame captures");
    const auto without = At(baseline, .5f, .5f);
    const auto with = At(withCascades, .5f, .5f);
    const auto stats = session.gpu.GetFrameStats();
    std::printf("  far-wall transfer RGB %d,%d,%d -> %d,%d,%d, cascade passes %u\n", without[0],
                without[1], without[2], with[0], with[1], with[2], stats.radianceCascadePasses);
    if (session.gpu.GetInfo().rayTracing) {
        Check(stats.radianceCascadePasses == 1 && stats.computeDispatches == 4,
              "three interval dispatches and a probe irradiance projection");
        Check(with[0] > without[0] + 15 && with[0] > with[1] + 15,
              "a non-local interval transfers indirect radiance to the receiver");
    } else {
        Check(stats.radianceCascadePasses == 0 && without == with,
              "unsupported devices keep their raster lighting");
    }
    Check(session.gpu.GetCounters().validationErrors == 0,
          "radiance cascade resources and barriers validate");
}

void RestirLighting() {
    std::printf("[restir_lighting]\n");
    GpuDeviceConfig config;
    config.vsync = false;
    Session session(128, 128, config, true);
    if (!session.ok)
        return Check(false, "session for ReSTIR lighting");

    constexpr std::array<Vertex3D, 4> quad{{{{-.5, -.5, 0}, {0, 0, 1}, {}},
                                            {{.5, -.5, 0}, {0, 0, 1}, {}},
                                            {{.5, .5, 0}, {0, 0, 1}, {}},
                                            {{-.5, .5, 0}, {0, 0, 1}, {}}}};
    constexpr std::array<uint32, 6> indices{0, 1, 2, 0, 2, 3};
    Mesh3D plane;
    if (!plane.Upload(&session.gpu, quad, indices))
        return Check(false, "ReSTIR fixture geometry uploads");

    Renderer3D::Context context(&session.gpu);
    PerspectiveCamera camera(ToRadians(35), 1, .1, 50);
    camera.SetPosition({0, 0, 3});
    camera.SetTarget({0, 0, 0});
    Render3DSettings settings;
    settings.imageBasedLighting = false;
    settings.shadows = false;
    settings.toneMapping = false;
    settings.sun.intensity = 0;
    settings.environmentColor = {0, 0, 0};
    auto white = Material3D{};
    white.alphaMode = MaterialAlphaMode::Opaque;
    auto red = white;
    red.albedoColor = {1, .015f, .015f, 1};
    const auto capture = [&](GpuFrameCapture& frame, std::span<const PointLight> points,
                             auto&& draw) {
        return CaptureFrame(session, frame, [&] {
            if (!session.gpu.Clear(0, 0, 0, 1))
                return false;
            context.BeginScene(camera, settings, 1, points);
            context.DrawMesh(plane, Mat4::Scale({4, 4, 1}), white);
            draw();
            context.EndScene();
            return context.GetLastAssetError().empty();
        });
    };

    // Equal proposal targets make the unoccluded sum exact for every seeded reservoir.
    const std::array lights{PointLight{.position = {3, 0, 2}, .intensity = 1.5f, .radius = 10.f},
                            PointLight{.position = {3, 0, 2}, .intensity = 1.5f, .radius = 10.f}};
    const auto blocker = [&] {
        context.DrawMesh(plane, Mat4::Translate({1.5, 0, 1}) * Mat4::Scale({.55, .55, 1}), white);
    };
    GpuFrameCapture baseline, restirDI;
    if (!capture(baseline, lights, blocker))
        return Check(false, "baseline point-lit frame captures");
    settings.rayTracing.restirDirect = true;
    if (!capture(restirDI, lights, blocker))
        return Check(false, "ReSTIR DI frame captures");
    const auto reference = At(baseline, .5f, .5f);
    const auto blocked = At(restirDI, .5f, .5f);
    auto stats = session.gpu.GetFrameStats();
    std::printf("  ReSTIR DI point shadow %d -> %d, passes %u\n", reference[0], blocked[0],
                stats.restirPasses);
    if (!session.gpu.GetInfo().rayTracing) {
        Check(stats.restirPasses == 0 && reference == blocked,
              "ReSTIR preserves raster output without ray query support");
        return;
    }
    Check(stats.restirPasses == 1 && stats.computeDispatches >= 2,
          "ReSTIR DI submits the candidate and spatial reuse dispatches");
    Check(reference[0] > blocked[0] + 40, "ReSTIR DI samples point-light visibility through TLAS");
    GpuFrameCapture unblocked;
    if (!capture(unblocked, lights, [] {}))
        return Check(false, "unoccluded ReSTIR point-light frame captures");
    Check(std::abs(int(reference[0]) - int(At(unblocked, .5f, .5f)[0])) <= 3,
          "weighted reservoirs recover the sum of unoccluded point lights");

    const auto redWall = [&] {
        // Behind the camera, this wall contributes through indirect transport only.
        context.DrawMesh(plane, Mat4::Translate({0, 0, 4.5}) * Mat4::Scale({24, 24, 1}), red);
    };
    settings.environmentColor = {.25, .25, .25};
    settings.rayTracing.restirDirect = false;
    GpuFrameCapture neutral, bounce;
    if (!capture(neutral, {}, redWall))
        return Check(false, "baseline indirect frame captures");
    settings.rayTracing.restirIndirect = true;
    if (!capture(bounce, {}, redWall))
        return Check(false, "ReSTIR GI frame captures");
    const auto original = At(neutral, .5f, .5f);
    const auto indirect = At(bounce, .5f, .5f);
    stats = session.gpu.GetFrameStats();
    std::printf("  ReSTIR GI bounce %d,%d,%d -> %d,%d,%d, passes %u\n", original[0], original[1],
                original[2], indirect[0], indirect[1], indirect[2], stats.restirPasses);
    Check(stats.restirPasses == 1 && indirect[0] > indirect[1] + 50,
          "ReSTIR GI transfers off-screen secondary diffuse radiance");
    Check(session.gpu.GetCounters().validationErrors == 0,
          "ReSTIR reservoir and lighting buffer barriers validate");
}

void LargeMipTexture() {
    Session session(96, 96);
    if (!session.ok)
        return Check(false, "8K mip session initializes");
    constexpr uint32 size = 8192;
    if (session.gpu.GetInfo().maxTextureSize < size) {
        std::printf("[large mip texture] adapter does not support 8192px textures\n");
        return;
    }
    constexpr uint64 expectedBytes = 357913940;
    const auto baseline = session.gpu.GetMemoryStats();
    GpuTexture texture;
    TextureSampling sampling;
    sampling.mipmaps = true;
    if (!texture.Initialize(&session.gpu, size, size, nullptr, TextureFilter::Linear,
                            TextureColorSpace::Srgb, sampling))
        return Check(false, "8K texture uploads with mipmaps enabled");
    const auto allocated = session.gpu.GetMemoryStats();
    Check(texture.GetWidth() == size && texture.GetHeight() == size &&
              texture.GetMipCount() == 14 && texture.GetStorageBytes() == expectedBytes &&
              allocated.allocationBytes - baseline.allocationBytes == expectedBytes &&
              allocated.liveTextures == baseline.liveTextures + 1,
          "GPU accounts for all 14 mip levels of the 8K texture");
    texture.Release();
    Check(session.gpu.WaitIdle() &&
              session.gpu.GetMemoryStats().allocationBytes == baseline.allocationBytes,
          "8K mip texture storage is reclaimed");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // keep output even if the process crashes
    // Optional: section names to run (basics misuse resize lifecycle frames drawing volume
    // resolutions wobble) and --wobble-frames N. No names = all sections.
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--wobble-frames" && i + 1 < argc)
            g_WobbleFrames = std::atoll(argv[++i]);
        else if (arg == "--frames-per-size" && i + 1 < argc)
            g_FramesPerSize = std::atoll(argv[++i]);
        else if (arg == "--heavy") {
            g_GrowParticles = 200000, g_GrowCubeLayers = 30, g_GrowSprites = 5000,
            g_GrowOverdraw = 60;
        } else if (arg == "--extreme") {
            g_GrowParticles = 500000, g_GrowCubeLayers = 60, g_GrowSprites = 20000,
            g_GrowOverdraw = 100;
            g_GrowSpeed = 4.0f;
            g_GrowStepMs = 150;
            g_GrowSpawnPerFrame = 25000;
        } else if (arg == "--until" && i + 1 < argc)
            g_RunSeconds = SecondsUntil(argv[++i]);
        else if (arg == "--total" && i + 1 < argc)
            g_TotalFrames = std::atoll(argv[++i]);
        else if (arg == "--stop-file" && i + 1 < argc)
            g_StopFile = argv[++i];
        else if (arg == "--scene-frames" && i + 1 < argc)
            g_SceneFrames = std::atoll(argv[++i]);
        else if (arg == "--capture-every" && i + 1 < argc)
            g_CaptureEvery = std::atoi(argv[++i]);
        else
            only.push_back(arg);
    }
    auto run = [&](const char* name, void (*section)()) {
        if (only.empty() || std::find(only.begin(), only.end(), name) != only.end())
            section();
    };
    if (!LuxEngine::Testing::HasGpuDisplay()) {
        std::printf("SKIP: no window-system display\n");
        return 77;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("FAIL: SDL video initialization: %s\n", SDL_GetError());
        return 1;
    }
    run("diagnostics", Diagnostics);
    run("basics", Basics);
    run("misuse", Misuse);
    run("ownership", Ownership);
    run("resize", Resizing);
    run("lifecycle", Lifecycle);
    run("frames", ManyFrames);
    run("drawing", Drawing);
    run("meshes", Meshes);
    run("scene3d", Scene3D);
    run("scene_submission", SceneSubmission);
    run("water_composite", WaterComposite);
    run("ocean_fft", OceanFft);
    run("ray_traced_lighting", RayTracedLighting);
    run("restir_lighting", RestirLighting);
    run("froxel_fog", FroxelFog);
    run("virtual_shadow_map", VirtualShadowMap);
    run("materials", MaterialModes);
    run("material_textures", MaterialTextures);
    run("radiance_cascades", RadianceCascades);
    run("large_mips", LargeMipTexture);
    run("volume", Volume);
    run("resolutions", Resolutions);
    run("wobble", Wobble);
    // Long-running: only when asked for by name.
    if (std::find(only.begin(), only.end(), "endurance") != only.end())
        Endurance();
    if (std::find(only.begin(), only.end(), "stress") != only.end())
        Stress();
    if (std::find(only.begin(), only.end(), "mix") != only.end())
        Mix();
    if (std::find(only.begin(), only.end(), "grow") != only.end())
        Grow();
    std::fflush(stdout);
    SDL_Quit();
    const int result = g_Failures == 0 ? 0 : 1;
    std::printf("%s: %d checks, %d failures\n", result == 0 ? "PASS" : "FAIL", g_Checks,
                g_Failures);
    return result;
}
