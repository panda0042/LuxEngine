#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace LuxEngine;

namespace {

void Check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

void TestSpectrum() {
    OceanSpectrum ocean;
    Check(!ocean.Sample({}) && !ocean.Update(0), "uninitialized ocean must reject sampling");
    OceanSpectrumSettings settings;
    settings.resolution = 32;
    settings.spectrumAmplitude = .02;
    settings.foamThreshold = 1;
    Check(ocean.Initialize(settings), "initialize ocean");
    auto invalid = settings;
    invalid.resolution = 30;
    Check(!ocean.Initialize(invalid) && ocean.Grid().size() == 32 * 32,
          "invalid reconfiguration must preserve the previous grid");
    Check(!ocean.Update(std::numeric_limits<double>::infinity()), "infinite time accepted");

    constexpr double time = 4;
    const Vec2 point{12.25, 6.8};
    Check(ocean.Update(time), "sample at finite time");
    const auto sample = ocean.Sample(point).value();
    const auto repeated = ocean.Sample(point + Vec2{-settings.patchLength, settings.patchLength});
    const auto moved =
        ocean.SampleAtSurface(point + Vec2{sample.displacement.x, sample.displacement.z});
    Check(repeated && (repeated->displacement - sample.displacement)
                              .Dot(repeated->displacement - sample.displacement) < 1e-12,
          "ocean grid is not periodic");
    Check(moved && std::abs(moved->displacement.y - sample.displacement.y) < .002,
          "displaced-coordinate surface query does not find the wave");

    const auto hasFoam = std::any_of(ocean.Grid().begin(), ocean.Grid().end(),
                                     [](const auto& p) { return p.foam > .1; });
    Check(hasFoam, "choppy spectrum does not create compression foam");
    double meanHeight = 0;
    for (const auto& p : ocean.Grid()) {
        meanHeight += p.displacement.y;
        Check(std::abs(p.normal.Dot(p.normal) - 1) < 1e-10 && p.normal.y >= 0,
              "ocean normal is not an upward unit vector");
        Check(std::abs(p.foam -
                       std::clamp((settings.foamThreshold - p.jacobian) / settings.foamSoftness, 0.,
                                  1.)) < 1e-10,
              "foam does not follow the displacement Jacobian");
    }
    Check(std::abs(meanHeight) < 1e-8, "the zero frequency introduced a sea level offset");
    constexpr double epsilon = 1e-4;
    Check(ocean.Update(time - epsilon), "sample before time");
    const auto before = ocean.Sample(point)->displacement;
    Check(ocean.Update(time + epsilon), "sample after time");
    const auto after = ocean.Sample(point)->displacement;
    const Vec3 velocityError = (after - before) * (.5 / epsilon) - sample.velocity;
    Check(velocityError.Dot(velocityError) < 1e-10,
          "spectral velocity does not match temporal displacement");

    settings.choppiness = 0;
    Check(ocean.Initialize(settings) && ocean.Update(time), "initialize height-only spectrum");
    for (const auto& p : ocean.Grid())
        Check(std::abs(p.displacement.x) < 1e-9 && std::abs(p.displacement.z) < 1e-9 &&
                  std::abs(p.jacobian - 1) < 1e-9 && p.foam < 1e-9,
              "height-only spectrum has lateral displacement or foam");

    settings.windSpeed = 0;
    Check(ocean.Initialize(settings), "initialize still water");
    for (const auto& p : ocean.Grid())
        Check(p.displacement.Dot(p.displacement) == 0 && p.normal.y == 1,
              "still water is not flat");

    constexpr double amplitude = .28, phase = .3, swellTime = .75;
    constexpr int32 modeX = 2, modeZ = -3;
    settings.swells[0] = {modeX, modeZ, amplitude, phase};
    const double frequency =
        std::sqrt(9.81 * 2 * std::numbers::pi * std::hypot(modeX, modeZ) / settings.patchLength);
    for (const uint32 resolution : {8u, 16u, 32u, 64u, 128u, 256u}) {
        settings.resolution = resolution;
        const double x = resolution - 2, z = 3;
        const double spacing = settings.patchLength / resolution;
        const double angle = phase + frequency * swellTime +
                             2 * std::numbers::pi * (x * modeX + z * modeZ) / resolution;
        Check(ocean.Initialize(settings) && ocean.Update(swellTime), "initialize diagonal swell");
        const auto swell = ocean.Sample({x * spacing, z * spacing}).value();
        Check(std::abs(swell.displacement.y - amplitude * std::cos(angle)) < 1e-10 &&
                  std::abs(swell.velocity.y + amplitude * frequency * std::sin(angle)) < 1e-10,
              "swell phase or velocity differs from its spectral mode across FFT rows or columns");
    }
    settings.swells[0].modeX = static_cast<int32>(settings.resolution / 2);
    Check(!ocean.Initialize(settings), "Nyquist swell mode must be rejected");
}

void TestBuoyancy() {
    OceanBuoyancySettings settings;
    settings.radius = .25;
    const double mass = 10;
    const double dt = 1. / 60;
    OceanSample water;
    Check(OceanBuoyancyImpulse(water, .3, {2, 0, 0}, mass, dt, settings).Dot({1, 1, 1}) == 0,
          "dry buoy received an impulse");
    const auto shallow = OceanBuoyancyImpulse(water, 0, {2, 0, 0}, mass, dt, settings);
    const auto deep = OceanBuoyancyImpulse(water, -.25, {2, 0, 0}, mass, dt, settings);
    Check(deep.y > shallow.y * 1.99 && shallow.y > mass * settings.gravity * dt &&
              deep.x < shallow.x && shallow.x < 0,
          "buoyancy does not track displaced volume or fluid drag");

    Scene scene;
    auto buoy = scene.CreateEntity("ocean buoy");
    auto& transform = buoy.GetComponent<TransformComponent>();
    auto& body = buoy.AddComponent<Rigidbody3DComponent>();
    body.type = BodyType::Dynamic;
    body.mass = static_cast<float>(mass);
    buoy.AddComponent<BoxCollider3DComponent>().size = {.5, .5, .5};
    Check(scene.AddImpulse3D(buoy.GetId(), shallow), "Jolt scene rejected buoyancy impulse");
    scene.OnUpdate(dt);
    Vec3 velocity;
    Check(scene.GetVelocity3D(buoy.GetId(), velocity) && velocity.y > 0 && transform.position.y > 0,
          "buoyancy impulse did not lift the Jolt body against gravity");
}

void TestCoastalBreaker() {
    const auto crest = SampleCoastalBreaker({-3.52, 0}, 2, 1.28f);
    const auto advanced = SampleCoastalBreaker({-5.32, 0}, 2.4, 1.28f);
    Check(crest.displacement.y > 1 && crest.displacement.x < -.6 && crest.foam > .4,
          "the breaking crest must lift, pitch landward and produce foam");
    Check(advanced.displacement.y > .8 && advanced.foam > crest.foam,
          "the breaker and its foam must progress shoreward");
    const auto leftLip = SampleCoastalBreaker({-4.5, 0}, 2, 1.6f);
    const auto rightLip = SampleCoastalBreaker({-4, 0}, 2, 1.6f);
    Check(rightLip.displacement.x + .5 < leftLip.displacement.x,
          "the crest must fold over the shoreward face at the coastal preset height");
    const auto highCrest = SampleCoastalBreaker({-3.52, 0}, 2, 1.6f);
    Check(highCrest.displacement.y > crest.displacement.y + .2 &&
              highCrest.displacement.x >= -1.1 * 1.6,
          "a higher breaker must grow without exceeding the horizontal solver interval");
    constexpr float surfHeight = 2.05f;
    const auto tallCrest = SampleCoastalBreaker({-3.52, 0}, 2, surfHeight);
    const auto barrelFace = SampleCoastalBreaker({-4.77, 0}, 2, surfHeight);
    const auto barrelLip = SampleCoastalBreaker({-4.27, 0}, 2, surfHeight);
    Check(tallCrest.displacement.y > 2.3 && barrelLip.displacement.y > 1 &&
              barrelLip.displacement.x + .5 < barrelFace.displacement.x &&
              tallCrest.displacement.x >= -1.1 * surfHeight,
          "the surf crest must noticeably overhang the taller plunging face");
    const auto fallingLip = SampleCoastalBreaker({-6.58, 0}, 2.6, surfHeight);
    const auto shoulder = SampleCoastalBreaker({-5.32, 0}, 2.6, surfHeight);
    Check(fallingLip.displacement.y < shoulder.displacement.y - .4 &&
              fallingLip.foam > shoulder.foam + .4,
          "the lip must fall ahead of the shoulder and leave thicker whitewater");
    constexpr float largeSwellHeight = 3.60f;
    const auto largeCrest = SampleCoastalBreaker({-3.52, 0}, 2, largeSwellHeight);
    const auto largeShoulder = SampleCoastalBreaker({-1.75, 0}, 2, largeSwellHeight);
    const auto largeFront = SampleCoastalBreaker({-4.5, 0}, 2, largeSwellHeight);
    const auto largeLip = SampleCoastalBreaker({-4, 0}, 2, largeSwellHeight);
    Check(largeCrest.displacement.y > 4 && largeShoulder.displacement.y > 2.8 &&
              largeLip.displacement.x + .5 < largeFront.displacement.x &&
              largeCrest.displacement.x >= -1.1 * largeSwellHeight,
          "a large swell must have a broad shoulder and an overturning crest within the solver "
          "interval");
    constexpr double landingTime = 2.72, lipX = -7.56;
    const auto landingLip = SampleCoastalBreaker({lipX, 0}, landingTime, largeSwellHeight);
    const double noseX = lipX + landingLip.displacement.x;
    const auto receivingWater =
        SampleCoastalBreaker({noseX - .24, 0}, landingTime, largeSwellHeight);
    Check(landingLip.displacement.x < -2 && landingLip.foam > .7 &&
              std::abs(landingLip.displacement.y - receivingWater.displacement.y) < .35,
          "the plunging lip must reach the water below while the crest is still folded");
    const auto spreadingFoam = SampleCoastalBreaker({-10.5, 0}, 3, largeSwellHeight);
    const auto fadingEdge = SampleCoastalBreaker({-13.999, 0}, 3, largeSwellHeight);
    Check(spreadingFoam.foam > .5 && spreadingFoam.displacement.y < .24 &&
              fadingEdge.displacement.Dot(fadingEdge.displacement) < 1e-8,
          "the large breaker must leave low spreading foam and fade smoothly at the strip edge");
    const auto runup = SampleCoastalBreaker({-11, 0}, 2.9, 1.28f);
    Check(runup.displacement.y < .18 && runup.foam > .45,
          "foam must move onto the shoreward side after the crest falls");
    const auto boundary = SampleCoastalBreaker({0, 32}, 2.4, 1.28f);
    const auto inactive = SampleCoastalBreaker({-3.52, 0}, 2, 0);
    Check(boundary.displacement.Dot(boundary.displacement) == 0 && boundary.foam == 0 &&
              inactive.displacement.Dot(inactive.displacement) == 0,
          "the breaker must fade out by the end of the coastal strip and allow disabling");

    for (float height : {0.f, 1.28f, largeSwellHeight, 4.f})
        for (double time : {-253.28, 0., 2., 2.72, 3.1, 258.72})
            for (double z : {-31., -23.5, -8., 0., 12., 30., 31.})
                for (double x = -14.5; x <= 16.5; x += .25)
                    Check(SampleCoastalBreakerHorizontal({x, z}, time, height) ==
                              SampleCoastalBreaker({x, z}, time, height).displacement.x,
                          "the position-only query must match full breaker deformation");
}

} // namespace

int main() {
    try {
        TestSpectrum();
        TestCoastalBreaker();
        TestBuoyancy();
        std::cout << "Ocean FFT, surface queries, Jacobian foam and Jolt buoyancy verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
