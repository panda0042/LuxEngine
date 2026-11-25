#include "Benchmark.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>

#include <psapi.h>
#endif

namespace Frontier {
namespace {
using Clock = std::chrono::steady_clock;
using LuxEngine::uint64;

double Milliseconds(Clock::duration elapsed) {
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

uint64 ProcessMemory() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS memory{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)))
        return uint64(memory.WorkingSetSize);
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key, line;
    while (status >> key) {
        if (key == "VmRSS:") {
            uint64 kib = 0;
            status >> kib;
            return kib * 1024;
        }
        std::getline(status, line);
    }
#endif
    return 0;
}

void String(std::ostream& output, std::string_view value) {
    output << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\')
            output << '\\';
        if (c < 32)
            output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c)
                   << std::dec;
        else
            output << c;
    }
    output << '"';
}

void OptionalBytes(std::ostream& output, uint64 bytes) {
    if (bytes)
        output << bytes;
    else
        output << "null";
}
} // namespace

Benchmark::Benchmark(unsigned warmup, unsigned count) : warmup_(warmup), count_(count) {
    samples_.reserve(count);
}

FrameSample& Benchmark::BeginFrame() {
    started_ = Clock::now();
    current_.gpuMs.reset();
    current_.gpuTimingFrame = 0;
    return current_;
}

bool Benchmark::EndFrame(const LuxEngine::GpuDevice& gpu, unsigned width, unsigned height,
                         bool ready) {
    const auto now = Clock::now();
    current_.cpuSubmitMs = Milliseconds(now - started_);
    if (lastPresent_ != Clock::time_point{})
        current_.presentIntervalMs = Milliseconds(now - lastPresent_);
    lastPresent_ = now;

    const auto frame = gpu.GetFrameStats();
    const auto memory = gpu.GetMemoryStats();
    if (gpu.GetInfo().gpuTimestamps && frame.gpuTimeMs > 0 &&
        frame.gpuTimingFrame > lastGpuTimingFrame_) {
        current_.gpuMs = frame.gpuTimeMs;
        current_.gpuTimingFrame = frame.gpuTimingFrame;
        lastGpuTimingFrame_ = frame.gpuTimingFrame;
    }
    current_.gpuWaitMs = frame.cpuWaitMs;
    current_.uploadCpuMs = frame.uploadCpuMs;
    current_.uploadWaitMs = frame.uploadWaitMs;
    current_.trackedGpuBytes = memory.allocationBytes;
    current_.deviceLocalBytes = gpu.GetInfo().memoryBudget ? memory.deviceLocalUsage : 0;
    current_.budgetBytes = gpu.GetInfo().memoryBudget ? memory.deviceLocalBudget : 0;
    current_.uploadBytes = frame.uploadBytes;
    current_.draws = frame.drawCalls;
    current_.dispatches = frame.computeDispatches;
    ++seen_;
    if (ready && ++readyFrames_ > warmup_ && count_ && current_.presentIntervalMs > 0) {
        if (samples_.empty()) {
            measuredWidth_ = width;
            measuredHeight_ = height;
        }
        if (width != measuredWidth_ || height != measuredHeight_)
            throw std::runtime_error("Framebuffer dimensions changed during benchmark measurement");
        current_.rssBytes = ProcessMemory();
        samples_.push_back(current_);
    }
    return count_ && samples_.size() == count_;
}

void Benchmark::Write(const std::filesystem::path& prefix, std::string_view scenario,
                      const LuxEngine::GpuDeviceInfo& device, std::string_view preset,
                      unsigned seed) const {
    if (!prefix.parent_path().empty())
        std::filesystem::create_directories(prefix.parent_path());
    auto csvPath = prefix;
    csvPath += ".csv";
    auto jsonPath = prefix;
    jsonPath += ".json";
    std::ofstream csv(csvPath), json(jsonPath);
    if (!csv || !json)
        throw std::runtime_error("Cannot open benchmark output");
    csv << std::setprecision(9);
    json << std::setprecision(9);
    csv << "frame,present_interval_ms,cpu_submit_to_present_ms,game_and_physics_ms,gpu_ms,gpu_"
           "timing_frame,cpu_gpu_wait_ms,"
           "rss_bytes,tracked_gpu_bytes,device_local_bytes,device_local_budget_bytes,upload_bytes,"
           "draw_calls,dispatches,"
           "physics_bodies,loaded_chunks,pending_chunks,visible_range_tree_meshes,generated_chunks,"
           "evicted_chunks,last_chunk_build_ms,upload_cpu_ms,upload_wait_ms,world_stream_ms,"
           "ground_cover_stream_ms,terrain_mesh_uploads,ground_cover_batch_uploads,"
           "ground_cover_tiles_pending\n";
    std::vector<double> ordered;
    unsigned gpuSamples = 0;
    double sumGPU = 0;
    uint64 maxRSS = 0, maxTracked = 0, maxLocal = 0;
    for (size_t i = 0; i < samples_.size(); ++i) {
        const auto& s = samples_[i];
        ordered.push_back(s.presentIntervalMs);
        maxRSS = std::max(maxRSS, s.rssBytes);
        maxTracked = std::max(maxTracked, s.trackedGpuBytes);
        maxLocal = std::max(maxLocal, s.deviceLocalBytes);
        csv << i + 1 << ',' << s.presentIntervalMs << ',' << s.cpuSubmitMs << ','
            << s.physicsAndGameMs << ',';
        if (s.gpuMs) {
            ++gpuSamples;
            sumGPU += *s.gpuMs;
            csv << *s.gpuMs << ',' << s.gpuTimingFrame;
        } else {
            csv << ',';
        }
        csv << ',' << s.gpuWaitMs << ',';
        if (s.rssBytes)
            csv << s.rssBytes;
        csv << ',' << s.trackedGpuBytes << ',';
        if (s.deviceLocalBytes)
            csv << s.deviceLocalBytes;
        csv << ',';
        if (s.budgetBytes)
            csv << s.budgetBytes;
        csv << ',' << s.uploadBytes << ',' << s.draws << ',' << s.dispatches << ',' << s.bodies
            << ',' << s.chunks.loaded << ',' << s.chunks.pending << ',' << s.chunks.trees << ','
            << s.chunks.built << ',' << s.chunks.evicted << ',' << s.chunks.lastBuildMs << ','
            << s.uploadCpuMs << ',' << s.uploadWaitMs << ',' << s.worldStreamMs << ','
            << s.groundCoverStreamMs << ',' << s.chunks.uploadedMeshes << ','
            << s.groundCoverBatches << ',' << s.groundCoverPending << '\n';
    }
    std::ranges::sort(ordered, std::greater{});
    const auto low = [&](double fraction) {
        const size_t count =
            std::max(size_t{1}, size_t(std::ceil(double(ordered.size()) * fraction)));
        return 1000. * double(count) /
               std::accumulate(ordered.begin(), ordered.begin() + std::ptrdiff_t(count), 0.);
    };
    const double average =
        std::accumulate(ordered.begin(), ordered.end(), 0.) / double(ordered.size());
    const auto stutters = std::ranges::count_if(samples_, [average](const FrameSample& sample) {
        return sample.presentIntervalMs >= std::max(33.3333, average * 2);
    });
    json << "{\n  \"scenario\": ";
    String(json, scenario);
    json << ",\n  \"device\": ";
    String(json, device.deviceName);
    json << ",\n  \"renderer\": ";
    String(json, device.deviceType);
    json << ",\n  \"preset\": ";
    String(json, preset);
    json << ",\n  \"seed\": " << seed << ",\n  \"width\": " << measuredWidth_
         << ",\n  \"height\": " << measuredHeight_
         << ",\n  \"startup_frames\": " << seen_ - readyFrames_
         << ",\n  \"warmup_frames\": " << warmup_ << ",\n  \"measured_frames\": " << samples_.size()
         << ",\n  \"average_fps\": " << 1000. / average
         << ",\n  \"one_percent_low_fps\": " << low(.01)
         << ",\n  \"point_one_percent_low_fps\": " << low(.001)
         << ",\n  \"mean_present_interval_ms\": " << average
         << ",\n  \"stutter_count\": " << stutters << ",\n  \"gpu_timing_samples\": " << gpuSamples
         << ",\n  \"mean_gpu_ms\": ";
    if (gpuSamples)
        json << sumGPU / gpuSamples;
    else
        json << "null";
    json << ",\n  \"peak_rss_bytes\": ";
    OptionalBytes(json, maxRSS);
    json << ",\n  \"peak_tracked_gpu_bytes\": ";
    OptionalBytes(json, maxTracked);
    json << ",\n  \"peak_device_local_bytes\": ";
    OptionalBytes(json, maxLocal);
    json << "\n}\n";
    csv.close();
    json.close();
    if (!csv || !json)
        throw std::runtime_error("Cannot write benchmark output");
}
} // namespace Frontier
