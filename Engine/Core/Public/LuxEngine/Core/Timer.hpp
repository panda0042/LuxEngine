#pragma once

#include <LuxEngine/Core/Core.hpp>
#include <chrono>

namespace LuxEngine {

class LUX_API Timer {
public:
    Timer();
    void Reset();
    double ElapsedSeconds() const;
    double ElapsedMilliseconds() const;

private:
    std::chrono::steady_clock::time_point start_;
};

class LUX_API FixedStepAccumulator {
public:
    static constexpr int MaxUpdatesPerFrame = 5;

    void AddElapsed(double seconds);
    bool Consume(double step);
    void DropBacklog(double step);
    void Reset();

private:
    double elapsed_ = 0;
};

} // namespace LuxEngine
