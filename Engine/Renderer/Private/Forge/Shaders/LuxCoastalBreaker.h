#ifndef LUX_COASTAL_BREAKER_H
#define LUX_COASTAL_BREAKER_H

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace LuxEngine::CoastalBreakerDetail {
using std::abs;
using std::exp;
using std::floor;
using std::max;
using std::min;
using std::sin;
#endif

struct CoastalBreakerSample {
    float horizontal;
    float height;
    float foam;
};

float BreakerSmooth(float edge0, float edge1, float value) {
    const float t = max(0.0f, min(1.0f, (value - edge0) / (edge1 - edge0)));
    return t * t * (3.0f - 2.0f * t);
}

CoastalBreakerSample EvaluateCoastalBreakerSample(float x, float z, float time, float height,
                                                  bool horizontalOnly) {
    CoastalBreakerSample wave;
    wave.horizontal = wave.height = wave.foam = 0.0f;
    if (x < -14.0f || x > 16.0f || abs(z) >= 31.0f || height <= 0.0f)
        return wave;

    const float cycle =
        time * 0.25f + z * 0.024f + sin(z * 0.24f) * 0.042f + sin(z * 0.61f) * 0.014f + 0.14f;
    const float age = cycle - floor(cycle);
    const float section = 0.5f + 0.5f * sin(z * 0.48f + sin(z * 0.18f) * 0.63f);
    const float envelope =
        BreakerSmooth(0.045f, 0.24f, age) * (1.0f - BreakerSmooth(0.825f, 0.99f, age)) *
        (1.0f - BreakerSmooth(21.0f, 31.0f, abs(z))) * BreakerSmooth(-14.0f, -12.4f, x) *
        (1.0f - BreakerSmooth(14.25f, 16.0f, x));
    const float tall = BreakerSmooth(2.05f, 3.60f, height);
    const float crestX = 8.0f - 18.0f * age + (0.65f + 0.33f * tall) * sin(z * 0.25f) -
                         (0.25f + 0.10f * tall) * sin(z * 0.53f);
    const float distance = x - crestX;
    if (envelope <= 0.0f)
        return wave;

    const float curl = BreakerSmooth(0.415f + 0.065f * section, 0.615f + 0.025f * section, age);
    const float plunge = BreakerSmooth(0.636f + 0.045f * section, 0.814f + 0.032f * section, age);
    const float relax = BreakerSmooth(0.753f + 0.018f * section, 0.927f + 0.015f * section, age);
    const float size = BreakerSmooth(1.0f, 2.10f, height);
    const float lip =
        (distance + 0.07f + 0.37f * plunge) /
        ((0.66f + 0.10f * section + 0.14f * size + 0.14f * plunge) * (1.0f + 0.39f * tall));
    const float crest = exp(-lip * lip);
    const float gain = height * envelope * (0.955f + 0.095f * sin(z * 0.69f));
    wave.horizontal = -gain * 1.035f * curl * (1.0f - 0.87f * relax) * crest;
    if (horizontalOnly)
        return wave;

    const float width = (distance < 0.0f ? 1.05f - 0.35f * curl : 2.66f + 0.35f * section) *
                        (1.0f + 0.12f * size) * (1.0f + 0.45f * tall);
    const float slope = distance / width;
    const float face = exp(-slope * slope);
    const float spread = BreakerSmooth(0.709f + 0.01f * section, 0.946f, age);
    const float runup = (distance + 0.72f + (2.52f + 0.68f * tall) * spread) /
                        (0.69f + 1.39f * spread + 0.18f * size + 0.23f * tall);
    const float whitewater = plunge * exp(-runup * runup);
    const float elevatedFace = face * (1.0f - 0.58f * relax) * (1.0f - 0.54f * plunge * crest) +
                               0.25f * curl * (1.0f - plunge) * crest;
    const float touchdown =
        BreakerSmooth(0.744f + 0.012f * section, 0.833f + 0.012f * section, age);
    const float lipFall = (0.22f + 0.27f * tall) * touchdown * crest;
    wave.height = gain * (max(0.0f, elevatedFace - lipFall) + 0.08f * whitewater);
    const float lipFoam = crest * curl * (0.44f + 0.44f * plunge) * (1.0f - 0.65f * relax);
    wave.foam =
        min(1.0f, (lipFoam + whitewater * 1.03f) * envelope * BreakerSmooth(0.0f, 0.20f, height));
    return wave;
}

CoastalBreakerSample EvaluateCoastalBreaker(float x, float z, float time, float height) {
    return EvaluateCoastalBreakerSample(x, z, time, height, false);
}

#ifdef __cplusplus
}
#endif
#endif
