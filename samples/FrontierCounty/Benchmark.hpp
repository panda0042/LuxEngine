#pragma once

#include "World.hpp"
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Frontier {

struct FrameSample {
    double presentIntervalMs = 0, cpuSubmitMs = 0, physicsAndGameMs = 0, gpuWaitMs = 0;
    std::optional<double> gpuMs;
    LuxEngine::uint64 gpuTimingFrame = 0;
    LuxEngine::uint64 rssBytes = 0, trackedGpuBytes = 0, deviceLocalBytes = 0, budgetBytes = 0;
    LuxEngine::uint64 uploadBytes = 0, draws = 0, dispatches = 0;
    unsigned bodies = 0;
    World::Stats chunks;
};

class Benchmark {
public:
    Benchmark(unsigned warmup, unsigned count);
    FrameSample& BeginFrame();
    bool EndFrame(const LuxEngine::GpuDevice& gpu, unsigned width, unsigned height);
    void Write(const std::filesystem::path& prefix, std::string_view scenario,
               std::string_view device, std::string_view preset, unsigned seed) const;

    double LastIntervalMs() const {
        return current_.presentIntervalMs;
    }

    unsigned MeasuredFrames() const {
        return unsigned(samples_.size());
    }

    unsigned PresentedFrames() const {
        return seen_;
    }

private:
    FrameSample current_;
    std::vector<FrameSample> samples_;
    std::chrono::steady_clock::time_point started_, lastPresent_;
    LuxEngine::uint64 lastGpuTimingFrame_ = 0;
    unsigned warmup_, count_, seen_ = 0, measuredWidth_ = 0, measuredHeight_ = 0;
};
} // namespace Frontier
