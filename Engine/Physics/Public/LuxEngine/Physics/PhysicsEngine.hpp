#pragma once

#include <LuxEngine/Core/Core.hpp>
#include <LuxEngine/Core/Math.hpp>
#include <memory>
#include <vector>

namespace LuxEngine {

enum class BodyType { Static, Dynamic, Kinematic };
enum class ColliderShape3D { Box, Capsule, TriangleMesh };

struct Rigidbody2DComponent {
    BodyType type = BodyType::Dynamic;
    Vec2 velocity{0.0f, 0.0f};
    float mass = 1.0f;
    float gravityScale = 1.0f;
    bool isGrounded = false;
};

struct BoxCollider2DComponent {
    Vec2 size{1.0f, 1.0f};
    Vec2 offset{0.0f, 0.0f};
    bool isTrigger = false;
};

struct Rigidbody3DComponent {
    BodyType type = BodyType::Dynamic;
    Vec3 velocity{0.0f, 0.0f, 0.0f};
    float mass = 1.0f;
    float gravityScale = 1.0f;
};

struct BoxCollider3DComponent {
    Vec3 size{1.0f, 1.0f, 1.0f};
    Vec3 offset{0.0f, 0.0f, 0.0f};
    bool isTrigger = false;
};

struct CapsuleCollider3DComponent {
    double radius = 0.5;
    double halfHeight = 0.5;
    Vec3 offset{};
    bool isTrigger = false;
};

struct TriangleMeshCollider3DData {
    std::vector<Vec3> vertices;
    std::vector<uint32> indices;
};

struct TriangleMeshCollider3DComponent {
    std::shared_ptr<const TriangleMeshCollider3DData> mesh;
};

struct CharacterController3DComponent {
    double radius = 0.35;
    double halfHeight = 0.55;
    double maxSlopeRadians = ToRadians(45);
    double stepHeight = 0.35;
};

struct Collision2DEvent {
    uint32 entityA = 0;
    uint32 entityB = 0;
    Vec2 normal{0.0f, 0.0f};
    bool isTrigger = false;
};

class LUX_API PhysicsEngine {
public:
    static void Init();
    static void Shutdown();

    static void SetGravity2D(const Vec2& gravity);
    static Vec2 GetGravity2D();

    static void SetGravity3D(const Vec3& gravity);
    static Vec3 GetGravity3D();
};

} // namespace LuxEngine
