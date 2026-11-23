#include <LuxEngine/Renderer/Water/OceanSpectrum.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
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

} // namespace

int main() {
    try {
        TestSpectrum();
        TestBuoyancy();
        std::cout << "Ocean FFT, surface queries, Jacobian foam and Jolt buoyancy verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
