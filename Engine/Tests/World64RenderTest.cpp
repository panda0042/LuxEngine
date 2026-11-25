#include "GpuTestEnvironment.hpp"
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Platform/Window.hpp>
#include <LuxEngine/Renderer/Renderer2D/Renderer2D.hpp>
#include <LuxEngine/Renderer/Renderer3D/Renderer3D.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <iostream>
#include <stdexcept>
using namespace LuxEngine;

int main() {
    if (!LuxEngine::Testing::HasGpuDisplay())
        return 77;
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }

    struct VideoLifetime {
        ~VideoLifetime() {
            SDL_Quit();
        }
    } video;

    Window window;
    WindowProps properties;
    properties.vulkan = true;
    properties.hidden = true;
    properties.width = 128;
    properties.height = 128;
    if (!window.Initialize(properties)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    GpuDevice gpu;
    GpuDeviceConfig config;
    config.vsync = false;
    config.validation = GpuValidation::On;
    if (!gpu.Initialize(window, config)) {
        std::cerr << gpu.GetLastError() << '\n';
        return 1;
    }
    try {
        Renderer3D::Context renderer(&gpu);
        Renderer3D::ScopedContext scoped(renderer);
        auto capture = [&](double origin, double distance, double size, double offscreenX = 0,
                           bool forceFallback = false) {
            PerspectiveCamera camera(ToRadians(60), 1, .1, std::max(1000000., distance * 10));
            camera.SetPosition({origin + .125, 0, distance});
            camera.SetTarget({origin + .125, 0, 0});
            for (int retry = 0; retry < 20; ++retry) {
                auto start = gpu.BeginFrame();
                if (start == GpuDevice::FrameStart::Ready)
                    break;
                if (start == GpuDevice::FrameStart::Error)
                    throw std::runtime_error(gpu.GetLastError());
                window.PollEvents();
            }
            if (!gpu.Clear(0, 0, 0, 1) || !gpu.RequestFrameCapture())
                throw std::runtime_error("World64 frame failed");
            Render3DSettings settings;
            settings.mode = Render3DMode::Pbr;
            settings.shadows = !forceFallback && distance < 10;
            settings.imageBasedLighting = false;
            settings.toneMapping = false;
            settings.sun.direction = {0, 0, -1};
            Material3D material;
            material.albedoColor = {1, .2f, .1f, 1};
            if (forceFallback)
                for (int pass = 0; pass < 16; ++pass) {
                    Renderer3D::BeginScene(camera, settings, 1);
                    Renderer3D::EndScene();
                    Renderer3D::BeginScene(camera, settings, 1);
                    Renderer3D::DrawCube({origin + .125, .9, 0}, {.15, .15, .15}, material);
                    Renderer3D::EndScene();
                    if (!renderer.GetLastAssetError().empty())
                        throw std::runtime_error("Empty scenes consumed the rendered pass budget");
                }
            Renderer3D::BeginScene(camera, settings, 1);
            if (offscreenX)
                for (int i = 0; i < 16384; ++i)
                    Renderer3D::DrawCube({origin + offscreenX, 0, 0}, {1, 1, 1}, material);
            Renderer3D::DrawCube({origin + .125, 0, 0}, {size, size, size}, material);
            Renderer3D::EndScene();
            if (forceFallback &&
                renderer.GetLastAssetError().find("Too many 3D scene passes") == std::string::npos)
                throw std::runtime_error("Expected the 17th scene pass to use the fallback");
            if (!gpu.EndFrame())
                throw std::runtime_error(gpu.GetLastError());
            GpuFrameCapture image;
            if (!gpu.ReadFrameCapture(image))
                throw std::runtime_error("World64 readback failed");
            return image;
        };
        auto normal = capture(0, 3, 1), large = capture(1e12, 3, 1),
             distant = capture(1e12, 100000, 20000);
        auto center = [](const auto& image) {
            return image.rgba.at((size_t(image.height / 2) * image.width + image.width / 2) * 4);
        };
        if (center(normal) < 50 || center(large) < 50 || center(distant) < 50)
            throw std::runtime_error("Nearby or distant large-world object disappeared");
        const auto fallback = center(capture(0, 3, 1, 0, true));
        if (fallback < 50 || center(capture(1e12, 3, 1, 0, true)) != fallback)
            throw std::runtime_error("Fallback rendering changed under large translation");
        if (center(capture(0, 3, 1, 10000)) != center(normal) ||
            center(capture(0, 3, 1, 20)) != center(normal))
            throw std::runtime_error("Offscreen meshes consumed the visible object's draw budget");
        size_t difference = 0;
        for (size_t i = 0; i < normal.rgba.size(); ++i)
            difference += std::abs(int(normal.rgba[i]) - int(large.rgba[i]));
        if (difference > normal.rgba.size() * 2)
            throw std::runtime_error("Camera-relative rendering changed under large translation");
        {
            Renderer2D::Context renderer2d(&gpu);
            Renderer2D::ScopedContext scoped2d(renderer2d);
            OrthographicCamera camera(-2, 2, -2, 2);
            camera.SetPosition({1e12 + .125, 1e12 + .25, 0});
            if (gpu.BeginFrame() != GpuDevice::FrameStart::Ready)
                throw std::runtime_error("2D frame failed");
            gpu.Clear(0, 0, 0, 1);
            gpu.RequestFrameCapture();
            Renderer2D::BeginScene(camera);
            Scene scene;
            auto map = scene.CreateEntity("Offscreen tiles");
            map.GetComponent<TransformComponent>().position = {1e12 + 10000, 1e12, 0};
            auto& tiles = map.AddComponent<TilemapComponent>();
            if (!tiles.Resize(256, 256))
                throw std::runtime_error("Could not create tilemap");
            std::fill(tiles.tiles.begin(), tiles.tiles.end(), 1);
            scene.OnRender();
            if (Renderer2D::GetStats().quadCount != 0)
                throw std::runtime_error("Offscreen tilemap submitted quads");
            auto& transform = map.GetComponent<TransformComponent>();
            transform.position = {1e12, 1e12, 0};
            transform.rotation.z = .3;
            scene.OnRender();
            const auto visibleTiles = Renderer2D::GetStats().quadCount;
            if (!visibleTiles || visibleTiles > 64)
                throw std::runtime_error(
                    "Partially visible rotated tilemap submitted wrong bounds");
            Renderer2D::DrawQuad(Mat4::Translate({1e12 + .125, 1e12 + .25, 0}), {1, 0, 0, 1});
            Renderer2D::EndScene();
            gpu.EndFrame();
            GpuFrameCapture image;
            if (!gpu.ReadFrameCapture(image) || center(image) < 100)
                throw std::runtime_error("2D large-world sprite disappeared");

            Scene sprites;
            auto sprite = sprites.CreateEntity("Camera centered sprite");
            auto& spriteTransform = sprite.GetComponent<TransformComponent>();
            spriteTransform.scale = {.14, .14, 1};
            auto& imageSprite = sprite.AddComponent<SpriteRendererComponent>();
            imageSprite.color = {1, 0, 0, 1};
            OrthographicCamera rotated(-.73, .73, -.51, .51);
            rotated.SetRotation(.31);
            const auto capture2D = [&](Scene& source, TransformComponent& placement, double origin,
                                       uint32 expectedQuads) {
                rotated.SetPosition({origin, origin, 0});
                placement.position = rotated.GetPosition();
                if (gpu.BeginFrame() != GpuDevice::FrameStart::Ready || !gpu.Clear(0, 0, 0, 1) ||
                    !gpu.RequestFrameCapture())
                    throw std::runtime_error("Rotated 2D frame failed");
                renderer2d.BeginScene(rotated);
                Vec2 minimum, maximum;
                if (!renderer2d.GetVisibleLocalBounds(placement.GetTransform(), minimum, maximum) ||
                    std::abs(minimum.x + maximum.x) > 1e-9 ||
                    std::abs(minimum.y + maximum.y) > 1e-9)
                    throw std::runtime_error("Rotated 2D local bounds shifted away from center");
                source.OnRender();
                renderer2d.EndScene();
                GpuFrameCapture result;
                if (renderer2d.GetStats().quadCount != expectedQuads ||
                    renderer2d.GetStats().drawCalls != 1 || !gpu.EndFrame() ||
                    !gpu.ReadFrameCapture(result))
                    throw std::runtime_error("Rotated 2D capture failed");
                return result.rgba;
            };
            for (const std::string texture : {"", LUX_WORLD64_TEXTURE_FIXTURE}) {
                imageSprite.texturePath = texture;
                const auto reference = capture2D(sprites, spriteTransform, 0, 1);
                if (capture2D(sprites, spriteTransform, 1e15, 1) != reference)
                    throw std::runtime_error("Rotated 2D sprite shifted or changed size");
            }

            Scene grid;
            auto tilemap = grid.CreateEntity("Small tiles");
            auto& tileTransform = tilemap.GetComponent<TransformComponent>();
            auto& cells = tilemap.AddComponent<TilemapComponent>();
            if (!cells.Resize(4, 2))
                throw std::runtime_error("Could not create small tilemap");
            cells.tileSize = .14;
            for (uint32 y = 0; y < cells.height; ++y)
                for (uint32 x = 0; x < cells.width; ++x)
                    cells.Set(x, y, static_cast<uint8_t>(1 + (x + y) % 3));
            const auto reference = capture2D(grid, tileTransform, 0, 8);
            if (capture2D(grid, tileTransform, 1e12, 8) != reference ||
                capture2D(grid, tileTransform, 1e15, 8) != reference)
                throw std::runtime_error("Tilemap cells shifted under large translation");
        }

        {
            std::string error;
            auto model = ModelAsset::LoadFbx(LUX_FBX_ANIMATED_FIXTURE, error);
            if (!model)
                throw std::runtime_error(error);
            ModelPose a, b, pose;
            if (!model->Evaluate(0, 0, a, error) || !model->Evaluate(.5, 0, b, error) ||
                !model->MixPoses({{&a, .5}, {&b, .5}}, pose, error))
                throw std::runtime_error(error);
            const auto& part = model->submeshes.at(0);
            const auto transform = pose.nodeWorldTransforms.at(part.node);
            Vec3 center{};
            for (const auto& vertex : pose.submeshes[0])
                center += transform.TransformPoint(vertex.position);
            center = center * (1.0 / pose.submeshes[0].size());
            double radius = 0;
            for (const auto& vertex : pose.submeshes[0]) {
                const auto delta = transform.TransformPoint(vertex.position) - center;
                radius = std::max(radius, std::hypot(delta.x, delta.y, delta.z));
            }
            const double distance = std::max(.01, radius * 3);
            PerspectiveCamera camera(ToRadians(60), 1, std::max(1e-5, distance * .001),
                                     std::max(1., distance * 10));
            camera.SetTarget(center);
            camera.SetPosition(center + Vec3{0, 0, distance});
            if (gpu.BeginFrame() != GpuDevice::FrameStart::Ready)
                throw std::runtime_error("Blended model frame failed");
            gpu.Clear(0, 0, 0, 1);
            gpu.RequestFrameCapture();
            Renderer3D::BeginScene(camera, DirectionalLight{});
            ModelPose incomplete;
            if (Renderer3D::DrawMeshAsset(LUX_FBX_ANIMATED_FIXTURE, transform, Material3D{}, 0, 0,
                                          .25, 1, &incomplete) ||
                Renderer3D::GetLastAssetError().empty())
                throw std::runtime_error(
                    "Missing pose submesh should be rejected with a diagnostic");
            incomplete.submeshes.emplace_back();
            if (Renderer3D::DrawMeshAsset(LUX_FBX_ANIMATED_FIXTURE, transform, Material3D{}, 0, 0,
                                          .25, 1, &incomplete))
                throw std::runtime_error("Pose with the wrong vertex count should be rejected");
            if (!Renderer3D::DrawMeshAsset(LUX_FBX_ANIMATED_FIXTURE, transform, Material3D{}, 0, 0,
                                           .25, 1, &pose, 1))
                throw std::runtime_error("Blended model upload failed");
            Renderer3D::EndScene();
            gpu.EndFrame();
            GpuFrameCapture image;
            if (!gpu.ReadFrameCapture(image))
                throw std::runtime_error("Blended model readback failed");
            size_t pixels = 0;
            for (size_t i = 0; i < image.rgba.size(); i += 4)
                if (image.rgba[i] > 5 || image.rgba[i + 1] > 5 || image.rgba[i + 2] > 5)
                    ++pixels;
            if (pixels < 8)
                throw std::runtime_error("Blended FBX pose did not render");
        }
        if (gpu.GetCounters().validationErrors)
            throw std::runtime_error("World64 Vulkan validation error");
        gpu.WaitIdle();
        std::cout << "64-bit 2D/3D camera-relative and distant rendering passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
