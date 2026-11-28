#include "Common/SampleApplication.hpp"
#include <LuxEngine/EntryPoint.hpp>
#include <stdexcept>

namespace le = LuxEngine;

namespace {
class Character3D final : public LuxSamples::SampleApplication {
public:
    static constexpr std::string_view Help =
        "3D 캐릭터: WASD·방향키로 이동하고 Space로 점프합니다.";

    explicit Character3D(LuxSamples::Options options)
        : SampleApplication("3D 캐릭터", "WASD / 방향키 이동   Space 점프", std::move(options)) {}

private:
    le::Scene scene_;
    le::Entity player_;
    le::PerspectiveCamera camera_;

    void Box(const std::string& name, le::Vec3 position, le::Vec3 size, le::Vec4 color,
             double tilt = 0) {
        auto box = scene_.CreateEntity(name);
        auto& pose = box.GetComponent<le::TransformComponent>();
        pose.position = position;
        pose.scale = size;
        pose.rotation.z = tilt;
        box.AddComponent<le::BoxCollider3DComponent>();
        box.AddComponent<le::MeshRendererComponent>().material.albedoColor = color;
    }

    void Reset() override {
        for (auto entity : scene_.GetEntities())
            scene_.DestroyEntity(entity);
        Box("바닥", {0, -.5, 0}, {18, 1, 14}, {.29f, .37f, .43f, 1});
        for (int index = 0; index < 3; ++index) {
            const double height = .21 * (index + 1);
            Box("계단", {index * .75, height * .5, -2}, {.75, height, 2}, {.36f, .60f, .55f, 1});
        }
        Box("완만한 경사", {3.5, .68, 1.8}, {4, .25, 2}, {.43f, .59f, .62f, 1}, .35);
        Box("벽", {4.5, 1, -2}, {.35, 2, 3}, {.61f, .45f, .39f, 1});
        player_ = scene_.CreateEntity("캐릭터");
        player_.GetComponent<le::TransformComponent>().position = {-3, 1, -2};
        player_.AddComponent<le::CharacterController3DComponent>();
        auto marker = scene_.CreateEntity("캐릭터 표시");
        marker.GetComponent<le::TransformComponent>().scale = {.65, 1.75, .65};
        marker.AddComponent<le::MeshRendererComponent>().material.albedoColor = {.9f, .64f, .27f,
                                                                                 1};
        if (!scene_.SetParent(marker.GetId(), player_.GetId(), false))
            throw std::runtime_error("캐릭터 표시를 연결할 수 없습니다.");
        LuxSamples::BindMovement();
        le::Input::BindAction("character-jump", le::LuxKey::Space);
    }

    void Step(float dt) override {
        const auto move = LuxSamples::Movement();
        if (!scene_.SetCharacterMotion3D(player_.GetId(), {move.x * 3, 0, -move.y * 3},
                                         le::Input::WasActionPressed("character-jump") ? 5 : 0))
            throw std::runtime_error("캐릭터 이동을 설정할 수 없습니다.");
        scene_.OnUpdate(dt);
        if (!scene_.GetLastPhysicsError3D().empty())
            throw std::runtime_error(scene_.GetLastPhysicsError3D());
    }

    void Draw(float) override {
        const auto position = scene_.GetWorldTransform(player_.GetId()).position;
        const double aspect = double(GetWindow().GetWidth()) / GetWindow().GetHeight();
        camera_.SetPerspective(le::ToRadians(50), aspect, .1, 100);
        camera_.SetPosition(position + le::Vec3{0, 7, 10});
        camera_.SetTarget(position + le::Vec3{0, 0, -1.5});
        const auto& settings = scene_.GetSettings();
        le::Renderer3D::BeginScene(camera_, settings.render3D, settings.exposure);
        scene_.OnRender3D();
        le::Renderer3D::EndScene();
    }
};
} // namespace

LUX_ENGINE_MAIN(LuxSamples::RunSample<Character3D>)
