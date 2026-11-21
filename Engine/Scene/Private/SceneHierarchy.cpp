#include "ScenePhysicsWorld.hpp"
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <LuxEngine/Scene/SceneManager.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <tuple>

namespace LuxEngine {
namespace {
bool Finite(const Vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool Finite(const TransformComponent& value) {
    return Finite(value.position) && Finite(value.rotation) && Finite(value.scale);
}

double Length(Vec3 value) {
    return std::sqrt(value.Dot(value));
}

bool Decompose(const Mat4& matrix, TransformComponent& value, bool requireTrs) {
    const auto* m = matrix.elements;
    value.position = {m[12], m[13], m[14]};
    Vec3 x{m[0], m[1], m[2]}, y{m[4], m[5], m[6]}, z{m[8], m[9], m[10]};
    value.scale = {Length(x), Length(y), Length(z)};
    constexpr double epsilon = 1e-6f;
    if (value.scale.x < epsilon || value.scale.y < epsilon || value.scale.z < epsilon) {
        value.rotation = {};
        return !requireTrs && Finite(value);
    }
    if (x.Dot(y.Cross(z)) < 0)
        value.scale.x = -value.scale.x;
    x = x * (1 / value.scale.x);
    y = y * (1 / value.scale.y);
    z = z * (1 / value.scale.z);
    // A rotated child below a non-uniformly scaled parent can contain shear. Keep-world edits
    // reject a non-TRS result rather than silently moving or stretching the object.
    if (requireTrs &&
        (std::abs(x.Dot(y)) > 1e-4f || std::abs(x.Dot(z)) > 1e-4f || std::abs(y.Dot(z)) > 1e-4f))
        return false;
    value.rotation.y = std::asin(std::clamp<double>(-x.z, -1.0f, 1.0f));
    if (std::abs(std::cos(value.rotation.y)) > 1e-5f) {
        value.rotation.x = std::atan2(y.z, z.z);
        value.rotation.z = std::atan2(x.y, x.x);
    } else {
        value.rotation.x = 0;
        value.rotation.z = std::atan2(-y.x, y.y);
    }
    return Finite(value);
}

bool InverseAffine(const Mat4& input, Mat4& result) {
    const auto* m = input.elements;
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6],
                 i = m[10];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
        return false;
    const double scale = 1 / determinant;
    auto* out = result.elements;
    out[0] = static_cast<double>((e * i - f * h) * scale);
    out[4] = static_cast<double>((c * h - b * i) * scale);
    out[8] = static_cast<double>((b * f - c * e) * scale);
    out[1] = static_cast<double>((f * g - d * i) * scale);
    out[5] = static_cast<double>((a * i - c * g) * scale);
    out[9] = static_cast<double>((c * d - a * f) * scale);
    out[2] = static_cast<double>((d * h - e * g) * scale);
    out[6] = static_cast<double>((b * g - a * h) * scale);
    out[10] = static_cast<double>((a * e - b * d) * scale);
    out[12] = -(out[0] * m[12] + out[4] * m[13] + out[8] * m[14]);
    out[13] = -(out[1] * m[12] + out[5] * m[13] + out[9] * m[14]);
    out[14] = -(out[2] * m[12] + out[6] * m[13] + out[10] * m[14]);
    return std::all_of(std::begin(result.elements), std::end(result.elements),
                       [](double v) { return std::isfinite(v); });
}

using AuthoredComponents =
    std::tuple<TagComponent, TransformComponent, SpriteRendererComponent, MeshRendererComponent,
               CameraComponent, Rigidbody2DComponent, BoxCollider2DComponent, Rigidbody3DComponent,
               BoxCollider3DComponent, TilemapComponent, AnimationComponent, AnimatorComponent,
               ModelAnimationComponent, ScriptComponent, SceneReferenceComponent>;

using SceneComponents = decltype(std::tuple_cat(
    AuthoredComponents{}, std::tuple<HierarchyComponent, InstanceSourceComponent>{}));

template <typename Tuple, typename Callback, size_t... Indices>
void ForEachType(Callback&& callback, std::index_sequence<Indices...>) {
    (callback.template operator()<std::tuple_element_t<Indices, Tuple>>(), ...);
}

template <typename Tuple, typename Callback> void ForEachType(Callback&& callback) {
    ForEachType<Tuple>(std::forward<Callback>(callback),
                       std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

const std::unordered_set<std::type_index>& SceneComponentTypes() {
    static const auto types = [] {
        std::unordered_set<std::type_index> result;
        ForEachType<SceneComponents>([&]<typename T> { result.emplace(typeid(T)); });
        return result;
    }();
    return types;
}
} // namespace

bool Scene::SetSettings(const SceneSettings& value) {
    const auto bounded = [](double v, double limit) {
        return std::isfinite(v) && std::abs(v) <= limit;
    };
    if (!bounded(value.clearColor.x, 1) || value.clearColor.x < 0 ||
        !bounded(value.clearColor.y, 1) || value.clearColor.y < 0 ||
        !bounded(value.clearColor.z, 1) || value.clearColor.z < 0 ||
        !bounded(value.clearColor.w, 1) || value.clearColor.w < 0 ||
        !std::isfinite(value.exposure) || value.exposure < 0 || value.exposure > 16 ||
        !bounded(value.gravity.x, 100000) || !bounded(value.gravity.y, 100000) ||
        !Finite(value.gravity3D) || !bounded(value.gravity3D.x, 100000) ||
        !bounded(value.gravity3D.y, 100000) || !bounded(value.gravity3D.z, 100000) ||
        !std::isfinite(value.fixedStep) || value.fixedStep < 1.0f / 240 || value.fixedStep > .25f ||
        value.physicsSubsteps < 1 || value.physicsSubsteps > 16)
        return false;
    if (!ValidateRender3DSettings(value.render3D))
        return false;
    const bool timingChanged = settings_.fixedStep != value.fixedStep ||
                               settings_.physicsSubsteps != value.physicsSubsteps;
    settings_ = value;
    if (timingChanged)
        ResetPhysicsTiming();
    return true;
}

uint32 Scene::GetParent(uint32 child) const {
    const auto* hierarchy = TryGetComponent<HierarchyComponent>(child);
    return hierarchy ? hierarchy->parent : 0;
}

std::vector<uint32> Scene::GetChildren(uint32 parent) const {
    std::vector<uint32> children;
    if (parent) {
        if (!IsAlive(parent))
            return children;
        const auto pool = pools_.find(typeid(HierarchyComponent));
        if (pool != pools_.end())
            for (const auto& [id, hierarchy] :
                 static_cast<const Pool<HierarchyComponent>&>(*pool->second).values)
                if (hierarchy.parent == parent && IsAlive(id))
                    children.push_back(id);
    } else {
        for (const auto id : entities_)
            if (!GetParent(id))
                children.push_back(id);
    }
    std::sort(children.begin(), children.end());
    return children;
}

std::vector<Entity> Scene::GetEntitiesInHierarchyOrder() {
    std::unordered_map<uint32, std::vector<uint32>> children;
    for (const auto& entity : GetEntities())
        children[GetParent(entity.GetId())].push_back(entity.GetId());
    std::vector<Entity> result;
    result.reserve(entities_.size());
    std::vector<uint32> pending(children[0].rbegin(), children[0].rend());
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        result.emplace_back(id, this);
        const auto& descendants = children[id];
        pending.insert(pending.end(), descendants.rbegin(), descendants.rend());
    }
    if (result.size() != entities_.size())
        throw std::logic_error("Invalid scene hierarchy");
    return result;
}

bool Scene::ValidateHierarchy() const {
    std::unordered_map<uint32, uint8_t> marks;
    marks.reserve(entities_.size());
    for (const uint32 id : entities_) {
        if (marks[id] == 2)
            continue;
        std::vector<uint32> chain;
        uint32 cursor = id;
        while (cursor) {
            if (!IsAlive(cursor) || marks[cursor] == 1)
                return false;
            if (marks[cursor] == 2)
                break;
            marks[cursor] = 1;
            chain.push_back(cursor);
            cursor = GetParent(cursor);
        }
        for (const uint32 item : chain)
            marks[item] = 2;
    }
    return true;
}

Mat4 Scene::GetWorldMatrix(uint32 id) const {
    RequireAlive(id);
    std::vector<uint32> chain;
    uint32 cursor = id;
    while (cursor) {
        if (!IsAlive(cursor) || chain.size() >= entities_.size())
            throw std::logic_error("Invalid scene hierarchy");
        chain.push_back(cursor);
        cursor = GetParent(cursor);
    }
    Mat4 matrix;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (const auto* transform = TryGetComponent<TransformComponent>(*it))
            matrix = matrix * transform->GetTransform();
    return matrix;
}

TransformComponent Scene::GetWorldTransform(uint32 id) const {
    RequireAlive(id);
    if (!GetParent(id)) {
        const auto* transform = TryGetComponent<TransformComponent>(id);
        return transform ? *transform : TransformComponent{};
    }
    TransformComponent result;
    if (!Decompose(GetWorldMatrix(id), result, false))
        throw std::logic_error("Invalid world transform");
    return result;
}

bool Scene::SetWorldTransform(uint32 id, const TransformComponent& value) {
    if (!HasComponent<TransformComponent>(id) || !Finite(value))
        return false;
    if (!GetParent(id)) {
        GetComponent<TransformComponent>(id) = value;
        return true;
    }
    Mat4 inverse;
    if (!InverseAffine(GetWorldMatrix(GetParent(id)), inverse))
        return false;
    const auto current = GetWorldTransform(id);
    const auto same = [](const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };
    if (same(value.rotation, current.rotation) && same(value.scale, current.scale)) {
        // Translation does not require decomposing a sheared linear matrix. Preserve the
        // original local rotation/scale and transform only the requested world position.
        const auto* m = inverse.elements;
        const auto p = value.position;
        const Vec3 localPosition{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
                                 m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                                 m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
        if (!Finite(localPosition))
            return false;
        GetComponent<TransformComponent>(id).position = localPosition;
        return true;
    }
    TransformComponent local;
    if (!Decompose(inverse * value.GetTransform(), local, true))
        return false;
    GetComponent<TransformComponent>(id) = local;
    return true;
}

bool Scene::SetParent(uint32 child, uint32 parent, bool keepWorld) {
    if (!IsAlive(child) || (parent && !IsAlive(parent)) || child == parent)
        return false;
    if (GetParent(child) == parent)
        return true;
    uint32 cursor = parent;
    size_t depth = 0;
    while (cursor) {
        if (cursor == child || !IsAlive(cursor) || ++depth > entities_.size())
            return false;
        cursor = GetParent(cursor);
    }
    TransformComponent local;
    if (keepWorld) {
        if (!HasComponent<TransformComponent>(child))
            return false;
        const auto world = GetWorldMatrix(child);
        Mat4 inverse;
        if ((parent && !InverseAffine(GetWorldMatrix(parent), inverse)) ||
            !Decompose(inverse * world, local, true))
            return false;
    }
    auto* hierarchy = TryGetComponent<HierarchyComponent>(child);
    if (!hierarchy)
        hierarchy = &AddComponent<HierarchyComponent>(child);
    hierarchy->parent = parent;
    if (keepWorld)
        GetComponent<TransformComponent>(child) = local;
    RepairReferenceLinks();
    return true;
}

void Scene::RepairReferenceLinks() {
    const auto descendant = [this](uint32 child, uint32 parent) {
        if (!parent || !IsAlive(parent) || child == parent)
            return false;
        size_t depth = 0;
        for (auto cursor = GetParent(child); cursor && ++depth <= entities_.size();
             cursor = GetParent(cursor))
            if (cursor == parent)
                return true;
        return false;
    };
    for (const auto id : entities_) {
        if (auto* reference = TryGetComponent<SceneReferenceComponent>(id))
            std::erase_if(reference->entityMap, [&](const auto& item) {
                return !IsAlive(item.second) || !descendant(item.second, id);
            });
        if (const auto* source = TryGetComponent<InstanceSourceComponent>(id))
            if (!HasComponent<SceneReferenceComponent>(source->instanceRoot) ||
                !descendant(id, source->instanceRoot))
                RemoveComponent<InstanceSourceComponent>(id);
    }
}

bool Scene::CopyEntityComponents(const Scene& source, uint32 sourceId, uint32 targetId) {
    if (!source.IsAlive(sourceId) || !IsAlive(targetId))
        return false;
    if (&source == this && sourceId == targetId)
        return true;
    const auto& supported = SceneComponentTypes();
    for (const auto& [type, pool] : source.pools_)
        if (pool->Contains(sourceId) && !supported.contains(type))
            return false;
    Scene prepared;
    try {
        prepared.entities_.insert(targetId);
        ForEachType<AuthoredComponents>([&]<typename T> {
            if (const auto* component = source.TryGetComponent<T>(sourceId))
                prepared.AddComponent<T>(targetId, *component);
            prepared.GetPool<T>();
            // Allocate before replacing any authored component. Prepared map nodes are moved
            // into the destination below; no vector/string allocations occur during commit.
            auto& destination = GetPool<T>().values;
            destination.reserve(destination.size() + 1);
        });
    } catch (...) {
        return false;
    }
    RemovePhysicsBody(targetId);
    ForEachType<AuthoredComponents>([&]<typename T> {
        auto& destination = GetPool<T>().values;
        destination.erase(targetId);
        auto node = prepared.GetPool<T>().values.extract(targetId);
        if (!node.empty())
            destination.insert(std::move(node));
    });
    return true;
}

bool Scene::ApplyState(Scene& prepared) {
    if (&prepared == this)
        return true;
    std::shared_ptr<int> nextLifetime, preparedLifetime;
    std::vector<uint32> removedBodies;
    try {
        std::string validation;
        if (!SerializeToString(validation) || !prepared.SerializeToString(validation))
            return false;
        nextLifetime = std::make_shared<int>(0);
        preparedLifetime = std::make_shared<int>(0);
        removedBodies.reserve(entities_.size());
        for (const auto id : entities_) {
            const bool had2D = HasComponent<BoxCollider2DComponent>(id) &&
                               (HasComponent<Rigidbody2DComponent>(id) ||
                                !HasComponent<BoxCollider3DComponent>(id));
            const bool has2D = prepared.HasComponent<BoxCollider2DComponent>(id) &&
                               (prepared.HasComponent<Rigidbody2DComponent>(id) ||
                                !prepared.HasComponent<BoxCollider3DComponent>(id));
            if (!prepared.HasComponent<TransformComponent>(id) || had2D != has2D ||
                (!has2D && !prepared.HasComponent<BoxCollider3DComponent>(id)))
                removedBodies.push_back(id);
        }
    } catch (...) {
        return false;
    }
    for (const auto id : removedBodies)
        RemovePhysicsBody(id);
    const bool timingChanged = settings_.fixedStep != prepared.settings_.fixedStep ||
                               settings_.physicsSubsteps != prepared.settings_.physicsSubsteps;
    pools_.swap(prepared.pools_);
    entities_.swap(prepared.entities_);
    referenceIdentities_.swap(prepared.referenceIdentities_);
    entitiesByReferenceIdentity_.swap(prepared.entitiesByReferenceIdentity_);
    std::swap(nextEntityId_, prepared.nextEntityId_);
    std::swap(settings_, prepared.settings_);
    for (const auto id : removedBodies)
        if (auto* body = TryGetComponent<Rigidbody2DComponent>(id))
            body->isGrounded = false;
    lifetime_ = std::move(nextLifetime);
    prepared.lifetime_ = std::move(preparedLifetime);
    if (timingChanged)
        ResetPhysicsTiming();
    std::erase_if(collisionEvents2D_, [this](const auto& event) {
        return !IsAlive(event.entityA) || !IsAlive(event.entityB);
    });
    std::erase_if(collisionEvents3D_, [this](const auto& event) {
        return !IsAlive(event.entityA) || !IsAlive(event.entityB);
    });
    updateBuffer_.clear();
    modelAssets_.clear();
    prepared.modelAssets_.clear();
    prepared.physics2D_.reset();
    prepared.physics3D_.reset();
    prepared.ResetPhysicsTiming();
    prepared.collisionEvents2D_.clear();
    prepared.collisionEvents3D_.clear();
    prepared.updateBuffer_.clear();
    return true;
}

void Scene::StageRegion(Scene& staged, const std::vector<uint32>& roots,
                        std::unordered_set<uint32>& anchors) const {
    const std::unordered_set<uint32> selected(roots.begin(), roots.end());
    std::unordered_map<uint32, std::vector<uint32>> children;
    for (const auto id : GetComponentEntityIds<HierarchyComponent>())
        children[GetParent(id)].push_back(id);
    for (const auto root : roots) {
        std::vector<uint32> chain{root};
        auto parent = GetParent(root);
        while (parent && !selected.contains(parent)) {
            if (!IsAlive(parent) || chain.size() >= entities_.size())
                throw std::logic_error("Invalid scene hierarchy");
            chain.push_back(parent);
            parent = GetParent(parent);
        }
        if (!parent)
            anchors.insert(chain.begin(), chain.end());
    }
    auto pending = roots;
    std::unordered_set<uint32> visited;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second)
            continue;
        if (const auto found = children.find(id); found != children.end())
            pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    visited.insert(anchors.begin(), anchors.end());
    staged.entities_ = std::move(visited);
    for (const auto id : staged.entities_)
        ForEachType<SceneComponents>([&]<typename T> {
            if (const auto* component = TryGetComponent<T>(id))
                staged.AddComponent<T>(id, *component);
        });
    for (const auto id : staged.entities_)
        if (const auto identity = referenceIdentities_.find(id);
            identity != referenceIdentities_.end())
            staged.SetReferenceIdentity(id, identity->second);
    staged.nextEntityId_ = nextEntityId_;
    staged.settings_ = settings_;
    staged.sourcePath_ = sourcePath_;
    staged.RepairReferenceLinks();
}

Scene::RegionReferences Scene::PrepareRegionReferences(Scene& staged,
                                                       const std::unordered_set<uint32>& affected) {
    RegionReferences references;
    const auto scripts = pools_.find(typeid(ScriptComponent));
    if (scripts == pools_.end())
        return references;

    const auto identityFor = [&](uint32 target) {
        if (const auto existing = referenceIdentities_.find(target);
            existing != referenceIdentities_.end())
            return existing->second;
        if (const auto prepared = references.identities.find(target);
            prepared != references.identities.end())
            return prepared->second;
        uint64 identity;
        do {
            identity = NewReferenceIdentity();
        } while (entitiesByReferenceIdentity_.contains(identity) ||
                 references.entitiesByIdentity.contains(identity));
        references.identities.emplace(target, identity);
        references.entitiesByIdentity.emplace(identity, target);
        if (staged.IsAlive(target) && !staged.SetReferenceIdentity(target, identity))
            throw std::runtime_error("씬 스트리밍 참조 ID가 중복되었습니다.");
        return identity;
    };

    auto& values = static_cast<Pool<ScriptComponent>&>(*scripts->second).values;
    for (auto& [id, script] : values) {
        auto* stagedScript = staged.TryGetComponent<ScriptComponent>(id);
        for (auto& [name, value] : script.fields) {
            auto* reference = std::get_if<ScriptEntityReference>(&value);
            if (!reference)
                continue;
            auto updated = *reference;
            if (updated.identity) {
                if (const auto resolved = ResolveScriptEntityReference(updated))
                    updated.id = resolved;
            } else if (IsAlive(updated.id)) {
                updated.identity = identityFor(updated.id);
            }
            if (updated == *reference)
                continue;
            if (stagedScript)
                stagedScript->fields.at(name) = updated;
            if (!affected.contains(id))
                references.fields.emplace_back(reference, updated);
        }
    }
    std::erase_if(references.identities, [&](const auto& entry) {
        if (!affected.contains(entry.first))
            return false;
        references.entitiesByIdentity.erase(entry.second);
        return true;
    });
    return references;
}

bool Scene::CommitRegion(Scene& staged, const std::unordered_set<uint32>& anchors,
                         const std::unordered_set<uint32>& affected, RegionReferences& references) {
    const auto inRegion = [&](uint32 id) { return !anchors.contains(id); };
    const auto regionSize = staged.entities_.size() - anchors.size();
    const auto newSize = entities_.size() - affected.size() + regionSize;
    if (newSize > 100000 || !staged.ValidateForSerialization())
        return false;

    const auto& supported = SceneComponentTypes();
    for (const auto& [type, pool] : pools_)
        if (!pool->Empty() && !supported.contains(type))
            for (const auto id : affected)
                if (!staged.IsAlive(id) && pool->Contains(id))
                    throw std::runtime_error(
                        "직렬화할 수 없는 컴포넌트가 있는 구역을 제거할 수 없습니다.");

    std::vector<std::pair<SceneReferenceComponent*, SceneReferenceComponent*>> changedAnchors;
    unsigned referenceCount = 0, inheritanceCount = 0;
    size_t referenceBytes = 0;
    const auto countReference = [&](const Scene& owner, uint32 id,
                                    const SceneReferenceComponent& reference) {
        ++referenceCount;
        referenceBytes +=
            reference.path.size() + reference.baseline.size() + reference.suspended.size();
        if (reference.mode == SceneReferenceMode::Inherited) {
            for (auto parent = owner.GetParent(id); parent; parent = owner.GetParent(parent))
                if (owner.HasComponent<SceneReferenceComponent>(parent))
                    return;
            ++inheritanceCount;
        }
    };
    for (const auto id : GetComponentEntityIds<SceneReferenceComponent>())
        if (!affected.contains(id) && !anchors.contains(id))
            countReference(*this, id, GetComponent<SceneReferenceComponent>(id));
    for (const auto id : staged.GetComponentEntityIds<SceneReferenceComponent>()) {
        auto& reference = staged.GetComponent<SceneReferenceComponent>(id);
        countReference(staged, id, reference);
        if (auto* current = TryGetComponent<SceneReferenceComponent>(id);
            current && !inRegion(id)) {
            std::unordered_set<uint32> sources;
            for (const auto& [source, local] : reference.entityMap)
                sources.insert(source);
            for (const auto& [source, local] : current->entityMap)
                if (!affected.contains(local) && sources.insert(source).second)
                    reference.entityMap.emplace_back(source, local);
            std::sort(reference.entityMap.begin(), reference.entityMap.end());
            changedAnchors.emplace_back(current, &reference);
        }
    }
    if (referenceCount > 128 || inheritanceCount > 1 || referenceBytes > 64u * 1024u * 1024u)
        return false;

    const auto reserveFor = [](auto& container, size_t size) {
        if (size > static_cast<double>(container.bucket_count()) * container.max_load_factor())
            container.reserve(size);
    };
    size_t incomingIdentities = references.identities.size();
    for (const auto& [id, identity] : staged.referenceIdentities_) {
        if (!inRegion(id))
            continue;
        if (const auto current = entitiesByReferenceIdentity_.find(identity);
            current != entitiesByReferenceIdentity_.end() && !affected.contains(current->second))
            return false;
        if (references.entitiesByIdentity.contains(identity))
            return false;
        ++incomingIdentities;
    }
    reserveFor(referenceIdentities_, referenceIdentities_.size() + incomingIdentities);
    reserveFor(entitiesByReferenceIdentity_,
               entitiesByReferenceIdentity_.size() + incomingIdentities);
    reserveFor(entities_, newSize);
    ForEachType<SceneComponents>([&]<typename T> {
        const auto found = staged.pools_.find(typeid(T));
        if (found == staged.pools_.end())
            return;
        const auto& values = static_cast<const Pool<T>&>(*found->second).values;
        const auto added = std::count_if(values.begin(), values.end(), [&](const auto& entry) {
            return inRegion(entry.first) && !affected.contains(entry.first);
        });
        auto& destination = GetPool<T>().values;
        reserveFor(destination, destination.size() + added);
    });

    for (const auto id : affected) {
        EraseReferenceIdentity(id);
        if (!staged.IsAlive(id)) {
            RemovePhysicsBody(id);
            for (auto& [type, pool] : pools_)
                pool->Erase(id);
            entities_.erase(id);
        } else {
            const auto uses2D = [id](const Scene& scene) {
                return scene.HasComponent<BoxCollider2DComponent>(id) &&
                       (scene.HasComponent<Rigidbody2DComponent>(id) ||
                        !scene.HasComponent<BoxCollider3DComponent>(id));
            };
            if (!staged.HasComponent<TransformComponent>(id) || uses2D(*this) != uses2D(staged) ||
                (!uses2D(staged) && !staged.HasComponent<BoxCollider3DComponent>(id))) {
                RemovePhysicsBody(id);
                if (auto* body = staged.TryGetComponent<Rigidbody2DComponent>(id))
                    body->isGrounded = false;
            }
        }
    }
    for (auto [current, replacement] : changedAnchors)
        *current = std::move(*replacement);
    ForEachType<SceneComponents>([&]<typename T> {
        const auto destination = pools_.find(typeid(T));
        if (destination == pools_.end())
            return;
        auto& values = static_cast<Pool<T>&>(*destination->second).values;
        for (const auto id : affected)
            values.erase(id);
        const auto found = staged.pools_.find(typeid(T));
        if (found == staged.pools_.end())
            return;
        auto& incoming = static_cast<Pool<T>&>(*found->second).values;
        for (auto it = incoming.begin(); it != incoming.end();) {
            const auto current = it++;
            if (inRegion(current->first))
                values.insert(incoming.extract(current));
        }
    });
    for (auto it = staged.entities_.begin(); it != staged.entities_.end();) {
        const auto current = it++;
        if (inRegion(*current) && !affected.contains(*current))
            entities_.insert(staged.entities_.extract(current));
    }
    for (auto it = staged.referenceIdentities_.begin(); it != staged.referenceIdentities_.end();) {
        const auto current = it++;
        if (inRegion(current->first)) {
            entitiesByReferenceIdentity_.insert(
                staged.entitiesByReferenceIdentity_.extract(current->second));
            referenceIdentities_.insert(staged.referenceIdentities_.extract(current));
        }
    }
    entitiesByReferenceIdentity_.merge(references.entitiesByIdentity);
    referenceIdentities_.merge(references.identities);
    for (const auto& [field, updated] : references.fields)
        *field = updated;
    RefreshScriptReferenceIds();
    nextEntityId_ = staged.nextEntityId_;
    if (settings_.fixedStep != staged.settings_.fixedStep ||
        settings_.physicsSubsteps != staged.settings_.physicsSubsteps)
        ResetPhysicsTiming();
    settings_ = std::move(staged.settings_);
    return true;
}

std::unordered_map<uint32, uint32> Scene::ImportEntities(const Scene& source,
                                                         const std::vector<uint32>& roots,
                                                         uint32 parent, bool preserveIds) {
    return ImportEntitiesInternal(source, roots, parent, preserveIds, false);
}

std::unordered_map<uint32, uint32> Scene::ImportEntitiesInternal(const Scene& source,
                                                                 const std::vector<uint32>& roots,
                                                                 uint32 parent, bool preserveIds,
                                                                 bool restoreLocal) {
    std::unordered_map<uint32, uint32> mapping;
    if (preserveIds && (!entities_.empty() || parent != 0))
        return mapping;
    if ((parent && !IsAlive(parent)) || !ValidateHierarchy() || !source.ValidateHierarchy())
        return mapping;
    std::unordered_map<uint32, std::vector<uint32>> children;
    std::vector<uint32> ordered(source.entities_.begin(), source.entities_.end());
    std::sort(ordered.begin(), ordered.end());
    for (const uint32 id : ordered)
        children[source.GetParent(id)].push_back(id);
    std::vector<uint32> pending = roots.empty() ? children[0] : roots;
    std::unordered_set<uint32> included;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (!source.IsAlive(id))
            return {};
        if (!included.insert(id).second)
            continue;
        const auto found = children.find(id);
        if (found != children.end())
            pending.insert(pending.end(), found->second.begin(), found->second.end());
    }
    if (included.size() > 100000 || entities_.size() > 100000 - included.size() ||
        included.size() > std::numeric_limits<uint32>::max() - nextEntityId_)
        return {};
    std::erase_if(ordered, [&](uint32 id) { return !included.contains(id); });
    const auto previousNext = nextEntityId_;
    std::vector<uint32> created;
    try {
        mapping.reserve(ordered.size());
        created.reserve(ordered.size());
        for (const auto id : ordered) {
            if (preserveIds)
                nextEntityId_ = id;
            const auto destination = CreateEntity();
            created.push_back(destination.GetId());
            mapping.emplace(id, destination.GetId());
        }
        for (const auto id : ordered) {
            const auto destination = mapping.at(id);
            if (!CopyEntityComponents(source, id, destination))
                throw std::runtime_error("Cannot import unsupported scene component");
            if (!restoreLocal && source.sourcePath_ != sourcePath_)
                SceneManager::RebaseImportedEntityPaths(*this, destination, source.sourcePath_);
            const auto originalParent = source.GetParent(id);
            const bool parentIncluded = mapping.contains(originalParent);
            HierarchyComponent hierarchy;
            if (const auto* original = source.TryGetComponent<HierarchyComponent>(id))
                hierarchy.group = original->group;
            hierarchy.parent = parentIncluded ? mapping.at(originalParent) : parent;
            if (hierarchy.parent || hierarchy.group || source.HasComponent<HierarchyComponent>(id))
                AddComponent<HierarchyComponent>(destination, hierarchy);
            if (!restoreLocal && !parentIncluded && originalParent &&
                HasComponent<TransformComponent>(destination)) {
                TransformComponent world;
                if (!Decompose(source.GetWorldMatrix(id), world, true))
                    throw std::runtime_error("Imported subtree has an unsupported sheared root");
                GetComponent<TransformComponent>(destination) = world;
            }
            if (const auto* origin = source.TryGetComponent<InstanceSourceComponent>(id))
                if (mapping.contains(origin->instanceRoot))
                    AddComponent<InstanceSourceComponent>(
                        destination, InstanceSourceComponent{mapping.at(origin->instanceRoot),
                                                             origin->sourceEntity});
        }
        std::unordered_map<uint64, uint64> identities;
        if (!restoreLocal) {
            for (const auto& [old, local] : mapping)
                if (auto* reference = TryGetComponent<SceneReferenceComponent>(local))
                    SceneManager::RebaseSuspended(*this, *reference, old, mapping, &identities);
            for (const auto& [old, local] : mapping)
                if (const auto sourceIdentity = source.referenceIdentities_.find(old);
                    sourceIdentity != source.referenceIdentities_.end())
                    if (const auto identity = identities.find(sourceIdentity->second);
                        identity != identities.end() &&
                        !SetReferenceIdentity(local, identity->second))
                        throw std::runtime_error("Duplicate imported reference identity");
        }
        for (const auto id : created)
            if (auto* script = TryGetComponent<ScriptComponent>(id))
                for (auto& [name, value] : script->fields)
                    if (auto* reference = std::get_if<ScriptEntityReference>(&value)) {
                        const auto target =
                            mapping.find(source.ResolveScriptEntityReference(*reference));
                        const auto identity = identities.find(reference->identity);
                        *reference = {target == mapping.end() ? 0 : target->second,
                                      identity == identities.end() ? 0 : identity->second};
                    }
        for (const auto id : created)
            if (auto* reference = TryGetComponent<SceneReferenceComponent>(id)) {
                std::erase_if(reference->entityMap,
                              [&](const auto& item) { return !mapping.contains(item.second); });
                for (auto& [sourceId, localId] : reference->entityMap)
                    localId = mapping.at(localId);
            }
    } catch (...) {
        for (const auto id : created) {
            EraseReferenceIdentity(id);
            for (auto& [type, pool] : pools_)
                pool->Erase(id);
            entities_.erase(id);
        }
        nextEntityId_ = previousNext;
        return {};
    }
    return mapping;
}
} // namespace LuxEngine
