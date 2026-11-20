#include <LuxEngine/Scene/Entity.hpp>

namespace LuxEngine {
Entity::Entity(uint32 id, Scene* scene)
    : id_(id), scene_(scene), lifetime_(scene ? scene->Lifetime() : std::weak_ptr<int>{}) {}

uint32 Entity::GetId() const {
    return id_;
}

Scene* Entity::GetScene() const {
    return lifetime_.expired() ? nullptr : scene_;
}

bool Entity::IsValid() const {
    return !lifetime_.expired() && scene_ && scene_->IsAlive(id_);
}

Entity::operator bool() const {
    return IsValid();
}

bool Entity::operator==(const Entity& other) const {
    return id_ == other.id_ && scene_ == other.scene_ && !lifetime_.owner_before(other.lifetime_) &&
           !other.lifetime_.owner_before(lifetime_);
}

bool Entity::operator!=(const Entity& other) const {
    return !(*this == other);
}

void Entity::RequireValid() const {
    if (!IsValid())
        throw std::out_of_range("Invalid entity");
}

Entity::Entity() = default;
} // namespace LuxEngine
