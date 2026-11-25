#include <LuxEngine/Core/Timer.hpp>
#include <algorithm>
#include <cmath>

namespace LuxEngine {

Timer::Timer() : start_(std::chrono::steady_clock::now()) {}

void Timer::Reset() {
    start_ = std::chrono::steady_clock::now();
}

double Timer::ElapsedSeconds() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
}

double Timer::ElapsedMilliseconds() const {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_)
        .count();
}

void FixedStepAccumulator::AddElapsed(double seconds) {
    if (std::isfinite(seconds) && seconds > 0)
        elapsed_ += std::min(seconds, .25);
}

bool FixedStepAccumulator::Consume(double step) {
    if (!std::isfinite(step) || step <= 0 || elapsed_ < step)
        return false;
    elapsed_ -= step;
    return true;
}

double FixedStepAccumulator::InterpolationAlpha(double step) const {
    if (!std::isfinite(step) || step <= 0)
        return 1.0;
    return std::clamp(elapsed_ / step, 0.0, 1.0);
}

void FixedStepAccumulator::DropBacklog(double step) {
    if (elapsed_ >= step)
        Reset();
}

void FixedStepAccumulator::Reset() {
    elapsed_ = 0;
}

} // namespace LuxEngine
