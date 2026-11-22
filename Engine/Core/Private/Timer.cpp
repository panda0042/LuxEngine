#include <LuxEngine/Core/Timer.hpp>
#include <SDL3/SDL_timer.h>
#include <algorithm>
#include <cmath>

namespace LuxEngine {

Timer::Timer() {
    Reset();
}

void Timer::Reset() {
    startTicksNs_ = SDL_GetTicksNS();
}

double Timer::ElapsedSeconds() const {
    const uint64 now = SDL_GetTicksNS();
    return static_cast<double>(now - startTicksNs_) / 1'000'000'000.0;
}

double Timer::ElapsedMilliseconds() const {
    const uint64 now = SDL_GetTicksNS();
    return static_cast<double>(now - startTicksNs_) / 1'000'000.0;
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

void FixedStepAccumulator::DropBacklog(double step) {
    if (elapsed_ >= step)
        Reset();
}

void FixedStepAccumulator::Reset() {
    elapsed_ = 0;
}

} // namespace LuxEngine
