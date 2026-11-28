#include "RenderDebugPanel.hpp"
#include <LuxEngine/Core/Event.hpp>
#include <LuxEngine/Platform/Input.hpp>
#include <LuxEngine/Platform/Window.hpp>
#include <LuxEngine/Renderer/RHI/GpuDevice.hpp>
#include <algorithm>
#include <array>
#include <cfloat>
#include <imgui.h>
#include <stdexcept>
#include <vector>

namespace Frontier {
namespace {
using namespace LuxEngine;

enum Effect : std::uint32_t {
    ToneMapping,
    TemporalAA,
    EdgeAA,
    AutoExposure,
    LocalExposure,
    Bloom,
    Grade,
    Lut,
    Focus,
    MotionBlur,
    Flare,
    Vignette,
    Grain,
    Chromatic,
    Distortion,
    Volumes,
    EffectCount
};

constexpr std::array<const char*, EffectCount> Labels{
    "ACES tone mapping", "Temporal AA",
    "Edge AA",           "Auto exposure",
    "Local exposure",    "HDR bloom",
    "Color grading",     "3D LUT",
    "Depth of field",    "Motion blur",
    "Lens flare",        "Vignette",
    "Film grain",        "Chromatic aberration",
    "Lens distortion",   "Post-process volumes",
};

struct View {
    const char* label;
    PostDebugView post = PostDebugView::Off;
    TemporalDebugView temporal = TemporalDebugView::Off;
};

constexpr std::array Views{
    View{"Final image"},
    View{"Exposure histogram", PostDebugView::ExposureHistogram},
    View{"HDR luminance", PostDebugView::HdrLuminance},
    View{"Texture mip", PostDebugView::TextureMip},
    View{"Base color", PostDebugView::BaseColor},
    View{"World normal", PostDebugView::WorldNormal},
    View{"Geometric normal", PostDebugView::GeometricNormal},
    View{"Roughness", PostDebugView::Roughness},
    View{"Metallic", PostDebugView::Metallic},
    View{"Material AO", PostDebugView::MaterialAO},
    View{"GTAO", PostDebugView::GTAO},
    View{"Bent normal", PostDebugView::BentNormal},
    View{"Direct lighting", PostDebugView::DirectLighting},
    View{"Indirect lighting", PostDebugView::IndirectLighting},
    View{"IBL", PostDebugView::IBL},
    View{"Emissive", PostDebugView::Emissive},
    View{"Scene depth", PostDebugView::SceneDepth},
    View{"Linear depth", PostDebugView::LinearDepth},
    View{"Depth pyramid", PostDebugView::DepthPyramid},
    View{"Shadow map", PostDebugView::ShadowMap},
    View{"Shadow cascade", PostDebugView::ShadowCascade},
    View{"Virtual shadow page", PostDebugView::VirtualShadowPage},
    View{"Contact shadows", PostDebugView::ContactShadows},
    View{"Cloud shadows", PostDebugView::CloudShadows},
    View{"ReSTIR reservoir", PostDebugView::RestirReservoir},
    View{"Ray noise", PostDebugView::RayNoise},
    View{"Denoised lighting", PostDebugView::DenoisedLighting},
    View{"SSR confidence", PostDebugView::SsrConfidence},
    View{"Volumetric fog", PostDebugView::VolumetricFog},
    View{"Water depth", PostDebugView::WaterDepth},
    View{"Cloud density", PostDebugView::CloudDensity},
    View{"Water reflection", PostDebugView::WaterReflection},
    View{"Terrain weights", PostDebugView::TerrainWeights},
    View{"Vegetation LOD", PostDebugView::VegetationLod},
    View{"Motion vectors", PostDebugView::Off, TemporalDebugView::Velocity},
    View{"Reactive mask", PostDebugView::Off, TemporalDebugView::Reactive},
    View{"History weight", PostDebugView::Off, TemporalDebugView::HistoryWeight},
    View{"History rejection", PostDebugView::Off, TemporalDebugView::Rejection},
    View{"Transparency", PostDebugView::Off, TemporalDebugView::Transparency},
};

constexpr std::uint32_t Bit(Effect effect) {
    return 1u << effect;
}

RenderVertex Vertex(const ImDrawVert& v) {
    constexpr float ByteToFloat = 1.f / 255.f;
    return {{v.pos.x, v.pos.y, 0},
            {((v.col >> IM_COL32_R_SHIFT) & 255u) * ByteToFloat,
             ((v.col >> IM_COL32_G_SHIFT) & 255u) * ByteToFloat,
             ((v.col >> IM_COL32_B_SHIFT) & 255u) * ByteToFloat,
             ((v.col >> IM_COL32_A_SHIFT) & 255u) * ByteToFloat},
            {v.uv.x, v.uv.y}};
}

RenderVertex Interpolate(const RenderVertex& a, const RenderVertex& b, double weight) {
    const auto mix = [weight](float x, float y) { return std::lerp(x, y, float(weight)); };
    return {{mix(a.position.x, b.position.x), mix(a.position.y, b.position.y), 0},
            {mix(a.color.x, b.color.x), mix(a.color.y, b.color.y), mix(a.color.z, b.color.z),
             mix(a.color.w, b.color.w)},
            {mix(a.uv.x, b.uv.x), mix(a.uv.y, b.uv.y)}};
}

// The public 2D draw API takes triangles. Clip only the partial triangles at
// ImGui's scroll/window boundaries and interpolate UVs for the font atlas.
void EmitClipped(std::array<RenderVertex, 8> polygon, const ImVec4& clip,
                 std::vector<RenderVertex>& vertices) {
    std::size_t count = 3;
    const std::array edges{clip.x, clip.y, -clip.z, -clip.w};
    for (std::size_t edge = 0; edge < edges.size() && count > 0; ++edge) {
        const auto distance = [&](const RenderVertex& vertex) {
            double position = edge % 2 ? vertex.position.y : vertex.position.x;
            return (edge < 2 ? position : -position) - edges[edge];
        };
        std::array<RenderVertex, 8> next{};
        std::size_t size = 0;
        auto previous = polygon[count - 1];
        double previousDistance = distance(previous);
        for (std::size_t index = 0; index < count; ++index) {
            const auto& vertex = polygon[index];
            const double currentDistance = distance(vertex);
            if ((currentDistance < 0) != (previousDistance < 0))
                next[size++] = Interpolate(previous, vertex,
                                           previousDistance / (previousDistance - currentDistance));
            if (currentDistance >= 0)
                next[size++] = vertex;
            previous = vertex;
            previousDistance = currentDistance;
        }
        polygon = next;
        count = size;
    }
    for (std::size_t index = 2; index < count; ++index)
        vertices.insert(vertices.end(), {polygon[0], polygon[index - 1], polygon[index]});
}

void DrawImGui(GpuDevice& gpu, const ImDrawData& data) {
    if (data.DisplaySize.x <= 0 || data.DisplaySize.y <= 0)
        return;
    const auto projection = Mat4::Orthographic(0, data.DisplaySize.x, data.DisplaySize.y, 0, -1, 1);
    std::vector<RenderVertex> triangles;
    for (int i = 0; i < data.CmdListsCount; ++i) {
        const ImDrawList& list = *data.CmdLists[i];
        for (const ImDrawCmd& cmd : list.CmdBuffer) {
            if (cmd.UserCallback)
                continue;
            triangles.clear();
            triangles.reserve(cmd.ElemCount);
            for (unsigned j = 0; j < cmd.ElemCount; j += 3) {
                std::array<RenderVertex, 8> polygon{};
                for (unsigned k = 0; k < 3; ++k) {
                    const auto index = list.IdxBuffer[cmd.IdxOffset + j + k] + cmd.VtxOffset;
                    polygon[k] = Vertex(list.VtxBuffer[index]);
                }
                EmitClipped(polygon, cmd.ClipRect, triangles);
            }
            if (!triangles.empty() &&
                !gpu.DrawTriangles(triangles, projection, static_cast<GpuTexture*>(cmd.GetTexID()),
                                   false))
                throw std::runtime_error(gpu.GetLastError());
        }
    }
}
} // namespace

RenderDebugPanel::~RenderDebugPanel() {
    Shutdown();
}

void RenderDebugPanel::Initialize(GpuDevice& gpu, const Render3DSettings& settings) {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    ImGui::StyleColorsDark();
    auto& style = ImGui::GetStyle();
    style.WindowRounding = 6.f;
    style.FrameRounding = 3.f;
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!font_.Initialize(&gpu, width, height, pixels))
        throw std::runtime_error("Could not upload the render panel font");
    io.Fonts->SetTexID(&font_);
    defaults_ = (settings.toneMapping ? Bit(ToneMapping) : 0u) |
                (settings.temporalAntialiasing ? Bit(TemporalAA) : 0u) |
                (settings.screenSpaceAntialiasing ? Bit(EdgeAA) : 0u) |
                (settings.autoExposure ? Bit(AutoExposure) : 0u) |
                (settings.localExposure > 0 ? Bit(LocalExposure) : 0u) |
                (settings.bloomIntensity > 0 ? Bit(Bloom) : 0u) | Bit(Grade) |
                (!settings.colorGradingLut.empty() ? Bit(Lut) : 0u) |
                (!settings.postVolumes.empty() ? Bit(Volumes) : 0u);
    effects_ = defaults_;
}

void RenderDebugPanel::Shutdown() {
    if (context_) {
        ImGui::DestroyContext(context_);
        context_ = nullptr;
    }
    font_.Release();
}

void RenderDebugPanel::FeedEvent(Event& event) {
    if (!context_)
        return;
    ImGui::SetCurrentContext(context_);
    auto& io = ImGui::GetIO();
    const auto [x, y] = Input::GetMousePosition();
    io.AddMousePosEvent(x, y);
    EventDispatcher dispatcher(event);
    dispatcher.Dispatch<MouseScrolledEvent>([&](MouseScrolledEvent& wheel) {
        io.AddMouseWheelEvent(wheel.GetXOffset(), wheel.GetYOffset());
        return false;
    });
    dispatcher.Dispatch<MouseButtonPressedEvent>([&](MouseButtonPressedEvent& button) {
        if (button.GetMouseButton() == int(LuxMouseButton::Left))
            io.AddMouseButtonEvent(0, true);
        return false;
    });
    dispatcher.Dispatch<MouseButtonReleasedEvent>([&](MouseButtonReleasedEvent& button) {
        if (button.GetMouseButton() == int(LuxMouseButton::Left))
            io.AddMouseButtonEvent(0, false);
        return false;
    });
}

void RenderDebugPanel::Apply(Render3DSettings& settings) const {
    const auto enabled = [&](Effect effect) { return (effects_ & Bit(effect)) != 0; };
    const auto& view = Views[diagnostic_];
    settings.postDebugView = view.post;
    settings.temporalDebugView = view.temporal;
    settings.toneMapping = enabled(ToneMapping);
    settings.temporalAntialiasing = enabled(TemporalAA) || view.temporal != TemporalDebugView::Off;
    settings.screenSpaceAntialiasing = enabled(EdgeAA) && !settings.temporalAntialiasing;
    settings.autoExposure = enabled(AutoExposure);
    settings.localExposure = enabled(LocalExposure) ? .25f : 0.f;
    settings.bloomIntensity = enabled(Bloom) ? .12f : 0.f;
    if (!enabled(Grade))
        settings.colorGrade = {};
    if (!enabled(Lut))
        settings.colorGradingStrength = 0;
    settings.physicalCamera.depthOfField = enabled(Focus);
    settings.physicalCamera.autofocusSpeed = enabled(Focus) ? 3.f : 0.f;
    if (enabled(Focus)) {
        settings.physicalCamera.fNumber = 4.f;
        settings.physicalCamera.shutterSeconds = 1.f / 2000.f;
        settings.apertureBlades = 7;
    }
    // Keep the motion demonstration visible even with the short exposure used by the DOF preset.
    settings.motionBlurShutter = enabled(MotionBlur) ? .5f : 0.f;
    settings.lensFlareStrength = enabled(Flare) ? .1f : 0.f;
    settings.vignette = enabled(Vignette) ? .15f : 0.f;
    settings.filmGrain = enabled(Grain) ? .04f : 0.f;
    settings.chromaticAberration = enabled(Chromatic) ? .25f : 0.f;
    settings.lensDistortion = enabled(Distortion) ? -.025f : 0.f;
    if (!enabled(Volumes)) {
        settings.postVolumes.clear();
    } else {
        for (auto& volume : settings.postVolumes) {
            if (!enabled(AutoExposure))
                volume.compensationEV.reset();
            if (!enabled(LocalExposure))
                volume.localExposure.reset();
            if (!enabled(Bloom))
                volume.bloomIntensity.reset();
            if (!enabled(Grade)) {
                volume.saturation.reset();
                volume.temperatureK.reset();
            }
            if (!enabled(Focus))
                volume.focusDistance.reset();
        }
    }
}

bool RenderDebugPanel::Draw(LuxEngine::Window& window, GpuDevice& gpu, bool open, bool cursorFree,
                            double dt) {
    const auto width = window.GetWidth(), height = window.GetHeight();
    if (!width || !height)
        return false;
    ImGui::SetCurrentContext(context_);
    auto& io = ImGui::GetIO();
    io.DisplaySize = {float(width), float(height)};
    unsigned drawableWidth = 0, drawableHeight = 0;
    if (window.GetFramebufferSize(drawableWidth, drawableHeight))
        io.DisplayFramebufferScale = {float(drawableWidth) / width, float(drawableHeight) / height};
    io.DeltaTime = float(dt);
    if ((open || cursorFree) && window.HasFocus()) {
        const auto [x, y] = Input::GetMousePosition();
        io.AddMousePosEvent(x, y);
        // Snapshot when the panel is closed, as a paused game leaves the menu button clickable.
        if (!open)
            io.AddMouseButtonEvent(0, Input::IsMouseButtonPressed(LuxMouseButton::Left));
    } else {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        io.AddMouseButtonEvent(0, false);
    }
    ImGui::NewFrame();
    bool changeOpen = false;
    ImGui::SetNextWindowPos({float(width) - 16, 16}, ImGuiCond_Always, {1, 0});
    if (open) {
        bool stillOpen = true;
        ImGui::SetNextWindowSize({std::clamp(float(width) - 24.f, 1.f, 388.f),
                                  std::clamp(float(height) - 28.f, 1.f, 476.f)},
                                 ImGuiCond_Always);
        constexpr auto flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;
        if (ImGui::Begin("Rendering Debug", &stillOpen, flags)) {
            if (ImGui::Button("Enable all")) {
                effects_ = ((1u << EffectCount) - 1u) & ~Bit(EdgeAA);
                diagnostic_ = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Disable all")) {
                effects_ = 0;
                diagnostic_ = 0;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset defaults")) {
                effects_ = defaults_;
                diagnostic_ = 0;
            }
            ImGui::Spacing();
            if (ImGui::BeginTable("Effects", ImGui::GetContentRegionAvail().x >= 302 ? 2 : 1)) {
                for (std::size_t i = 0; i < Labels.size(); ++i) {
                    ImGui::TableNextColumn();
                    const auto effect = static_cast<Effect>(i);
                    const auto bit = Bit(effect);
                    bool checked = (effects_ & bit) != 0;
                    if (ImGui::Checkbox(Labels[i], &checked)) {
                        effects_ = checked ? effects_ | bit : effects_ & ~bit;
                        if (checked && (effect == TemporalAA || effect == EdgeAA))
                            effects_ &= ~Bit(effect == TemporalAA ? EdgeAA : TemporalAA);
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##view", Views[diagnostic_].label)) {
                for (std::size_t i = 0; i < Views.size(); ++i) {
                    if (ImGui::Selectable(Views[i].label, i == diagnostic_))
                        diagnostic_ = i;
                    if (i == diagnostic_)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("Diagnostic view  |  Tab to return to gameplay");
        }
        ImGui::End();
        changeOpen = !stillOpen;
    } else {
        ImGui::Begin("##OpenRenderDebug", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
        changeOpen = ImGui::Button("Rendering Debug  [Tab]");
        ImGui::End();
    }
    ImGui::Render();
    DrawImGui(gpu, *ImGui::GetDrawData());
    return changeOpen;
}
} // namespace Frontier
