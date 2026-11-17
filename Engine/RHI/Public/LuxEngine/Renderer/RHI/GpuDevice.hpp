#pragma once
#include <LuxEngine/Core/Core.hpp>
#include <LuxEngine/Core/Math.hpp>
#include <LuxEngine/Renderer/RHI/Buffer.hpp>
#include <LuxEngine/Renderer/RHI/Texture.hpp>
#include <span>
#include <string>
#include <vector>

namespace LuxEngine {
class Window;

namespace Rhi {
class RenderBackend;
}

// GPU-local transfer values. World/scene math uses double; renderers rebase before packing.
struct LUX_API GpuVec2 {
    float x = 0, y = 0;
    GpuVec2();
    GpuVec2(double x, double y);
    GpuVec2(Vec2 value);
    operator Vec2() const;
};

struct LUX_API GpuVec3 {
    float x = 0, y = 0, z = 0;
    GpuVec3();
    GpuVec3(double x, double y, double z);
    GpuVec3(Vec3 value);
    operator Vec3() const;
};

struct RenderVertex {
    GpuVec3 position;
    Vec4 color;
    GpuVec2 uv;
    GpuVec3 normal{};
};

enum class GpuValidation { Auto, Off, On, Synchronization };

enum class GpuStatus { Uninitialized, Ready, OutOfMemory, SurfaceLost, Timeout, DeviceLost, Error };

struct GpuDeviceConfig {
    // Clamped to 1..8. Swapchain image count is selected independently by the platform.
    uint32 framesInFlight = 2;
    bool vsync = true;
    GpuValidation validation = GpuValidation::Auto;
    std::string shaderDirectory;
    std::string preferredDevice;
    uint64 stagingFlushBytes = 64ull << 20;
    uint64 maxFrameVertexBytes = 256ull << 20;
    uint64 gpuTimeoutNs = 5'000'000'000ull;
};

struct GpuDeviceInfo {
    std::string deviceName;
    std::string deviceType;
    uint32 apiVersion = 0;
    uint32 driverVersion = 0;
    uint32 graphicsQueueFamily = 0;
    uint32 presentQueueFamily = 0;
    uint32 computeQueueFamily = 0;
    uint32 transferQueueFamily = 0;
    uint32 maxTextureSize = 0;
    uint32 framesInFlight = 0;
    bool timelineSemaphores = false;
    bool memoryBudget = false;
    bool gpuTimestamps = false;
    bool validation = false;
    bool synchronizationValidation = false;
    bool presentFences = false;
};

struct GpuFrameStats {
    uint64 frameIndex = 0;
    uint32 drawCalls = 0;
    uint32 pipelineBinds = 0;
    uint32 descriptorBinds = 0;
    uint32 pushConstantUpdates = 0;
    uint32 uniformBinds = 0;
    uint32 submissions = 0;
    uint64 vertexBytes = 0;
    uint64 uploadBytes = 0;
    double cpuWaitMs = 0.0;
    double gpuTimeMs = -1.0;
};

struct GpuDeviceCounters {
    uint64 framesSubmitted = 0;
    uint64 framesSkipped = 0;
    uint64 submissions = 0;
    uint64 swapchainRecreations = 0;
    uint64 pipelinesCreated = 0;
    uint64 presentFencesSubmitted = 0;
    uint64 presentFencesWaited = 0;
    // Messages from the shared Vulkan instance since this device initialized. Concurrent
    // devices sharing an instance observe the same messages, but never inherit older history.
    uint32 validationErrors = 0;
    uint32 validationWarnings = 0;
};

struct GpuMemoryStats {
    uint64 deviceLocalUsage = 0;
    uint64 deviceLocalBudget = 0;
    // Tracked resource payload, including buffers awaiting GPU completion. Driver allocations
    // and swapchain storage are not measured by the Forge bridge.
    uint64 allocationBytes = 0;
    uint32 allocationCount = 0;
    uint32 liveBuffers = 0;
    uint32 liveTextures = 0;
    uint32 pendingDestructions = 0;
    uint32 descriptorPools = 0;
    uint64 pendingStagingBytes = 0;
};

struct GpuPointLight {
    Vec3 position;
    float radius = 0.0f;
    Vec3 color;
    float intensity = 0.0f;
};

struct GpuLighting {
    Vec3 direction;
    Vec3 color;
    float intensity = 0.0f;
    std::span<const GpuPointLight> points;
    Vec3 camera;
    bool pbr = false;
};

struct GpuFrameCapture {
    uint32 width = 0;
    uint32 height = 0;
    std::vector<uint8> rgba;
};

class LUX_API GpuDevice {
public:
    enum class FrameStart { Ready, Skipped, Error };

    GpuDevice();
    ~GpuDevice();
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;
    GpuDevice(GpuDevice&&) = delete;
    GpuDevice& operator=(GpuDevice&&) = delete;

    bool Initialize(Window& window, const GpuDeviceConfig& config = {});
    void Shutdown();
    FrameStart BeginFrame();
    bool Clear(float r, float g, float b, float a);
    bool EndFrame();
    bool DrawTriangles(std::span<const RenderVertex> vertices, const Mat4& viewProjection,
                       GpuTexture* texture = nullptr, bool depth = false);
    bool SetVSync(bool enabled);
    void NotifyResize();
    bool FlushUploads();
    bool WaitIdle();
    bool RequestFrameCapture();
    bool ReadFrameCapture(GpuFrameCapture& capture);

    bool IsReady() const;
    GpuStatus GetStatus() const;
    GpuStatus GetLastFailure() const;
    const std::string& GetLastError() const;
    const GpuDeviceInfo& GetInfo() const;
    GpuFrameStats GetFrameStats() const;
    GpuDeviceCounters GetCounters() const;
    GpuMemoryStats GetMemoryStats() const;

private:
    friend class GpuBuffer;
    friend class GpuTexture;
    friend class Renderer2D;
    friend class Renderer3D;
    bool SetLighting(const GpuLighting& lighting);
    bool DrawIndexed(const GpuBuffer& vertices, const GpuBuffer& indices, uint32 indexCount,
                     const Mat4& viewProjection, const Mat4& model, const Vec4& albedo,
                     GpuTexture* texture = nullptr, float metallic = 0, float roughness = .5f,
                     bool pbr = false);
    bool DrawTexturedTriangles(std::span<const RenderVertex> vertices, const Mat4& viewProjection,
                               uint64 textureHandle, bool depth);
    Ref<Rhi::RenderBackend> backend_;
};

LUX_API const char* ToString(GpuStatus status);
} // namespace LuxEngine
