#include <LuxEditor/Viewport/EngineSceneRenderer.hpp>
#include <LuxEditor/Viewport/GameViewport.hpp>
#include <LuxEditor/Viewport/ViewportCamera3D.hpp>
#include <LuxEngine/Assets/AssetManager.hpp>
#include <LuxEngine/Platform/Input.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <LuxEngine/Scene/Scene.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace LuxEditor {
namespace {
LuxUI::Color Color(LuxEngine::Vec4 color) {
    return LuxUI::Color::fromRgbF(std::clamp(color.x, 0.f, 1.f), std::clamp(color.y, 0.f, 1.f),
                                  std::clamp(color.z, 0.f, 1.f), std::clamp(color.w, 0.f, 1.f));
}

LuxEngine::Vec3 Transform(const LuxEngine::Mat4& matrix, LuxEngine::Vec3 point) {
    const auto* m = matrix.elements;
    return {m[0] * point.x + m[4] * point.y + m[8] * point.z + m[12],
            m[1] * point.x + m[5] * point.y + m[9] * point.z + m[13],
            m[2] * point.x + m[6] * point.y + m[10] * point.z + m[14]};
}

LuxUI::PointF Project2D(const LuxEngine::Mat4& matrix, LuxUI::SizeF size, LuxEngine::Vec3 point) {
    const auto clip = Transform(matrix, point);
    return {(clip.x + 1) * size.width() * .5, (1 - clip.y) * size.height() * .5};
}

std::pair<int, int> SpriteCell(int size, LuxEngine::uint32 cell, LuxEngine::uint32 count) {
    const auto start = static_cast<int>(std::ceil(double(cell) * size / count - .5));
    const auto end = static_cast<int>(std::ceil(double(cell + 1) * size / count - .5));
    const int first = std::clamp(start, 0, size - 1);
    return {first, std::clamp(end, first + 1, size)};
}

void TintSprite(LuxUI::Image& image, const std::array<float, 4>& tint) {
    for (int y = 0; y < image.height(); ++y) {
        auto* pixels = image.scanLine(y);
        for (int x = 0; x < image.width(); ++x)
            for (size_t channel = 0; channel < tint.size(); ++channel) {
                auto& value = pixels[static_cast<size_t>(x) * 4 + channel];
                value = static_cast<unsigned char>(
                    std::clamp(float(value) * tint[channel], 0.f, 255.f) + .5f);
            }
    }
}

std::optional<LuxEngine::LuxMouseButton> MouseButton(LuxUI::Flags::MouseButton button) {
    switch (button) {
    case LuxUI::Flags::LeftButton:
        return LuxEngine::LuxMouseButton::Left;
    case LuxUI::Flags::MiddleButton:
        return LuxEngine::LuxMouseButton::Middle;
    case LuxUI::Flags::RightButton:
        return LuxEngine::LuxMouseButton::Right;
    case LuxUI::Flags::XButton1:
        return LuxEngine::LuxMouseButton::X1;
    case LuxUI::Flags::XButton2:
        return LuxEngine::LuxMouseButton::X2;
    default:
        return std::nullopt;
    }
}

void ClearInput() {
    LuxEngine::Input::ClearExternalKeys();
    LuxEngine::Input::ClearExternalMouse();
}
} // namespace

GameViewport::GameViewport(LuxUI::Widget* parent)
    : LuxUI::Widget(parent), renderer_(std::make_shared<EngineSceneRenderer>()) {
    setObjectName(LUX_UI_TEXT("gameViewport"));
    setFocusPolicy(LuxUI::Flags::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(240, 160);
    setAutoFillBackground(false);
}

GameViewport::~GameViewport() {
    ClearInput();
}

void GameViewport::SetScene(LuxEngine::Scene* scene) {
    const auto lifetime = scene ? scene->Lifetime() : std::weak_ptr<int>{};
    if (!sceneProvider_ && scene_ == scene && !lifetime.owner_before(sceneLifetime_) &&
        !sceneLifetime_.owner_before(lifetime)) {
        update();
        return;
    }
    ClearInput();
    sceneProvider_ = {};
    scene_ = scene;
    sceneLifetime_ = lifetime;
    paused_ = false;
    inputEnabled_ = false;
    frame_ = {};
    softwareTextures_.clear();
    softwareTextureBytes_ = 0;
    update();
}

void GameViewport::SetSceneProvider(std::function<LuxEngine::Scene*()> provider) {
    ClearInput();
    scene_ = nullptr;
    sceneLifetime_.reset();
    sceneProvider_ = std::move(provider);
    inputEnabled_ = false;
    paused_ = false;
    frame_ = {};
    softwareTextures_.clear();
    softwareTextureBytes_ = 0;
    update();
}

LuxEngine::Scene* GameViewport::GetScene() const noexcept {
    if (sceneProvider_) {
        try {
            return sceneProvider_();
        } catch (...) {
            return nullptr;
        }
    }
    return sceneLifetime_.expired() ? nullptr : scene_;
}

void GameViewport::SetInputEnabled(bool enabled) {
    if (inputEnabled_ == enabled)
        return;
    ClearInput();
    inputEnabled_ = enabled;
}

bool GameViewport::IsInputEnabled() const noexcept {
    return inputEnabled_;
}

void GameViewport::SetPaused(bool paused) {
    if (paused_ == paused)
        return;
    paused_ = paused;
    ClearInput();
    update();
}

bool GameViewport::IsPaused() const noexcept {
    return paused_;
}

void GameViewport::RefreshScene() {
    update();
}

std::shared_ptr<EngineSceneRenderer> GameViewport::GetRenderer() const {
    return renderer_;
}

bool GameViewport::event(LuxUI::Event* event) {
    if (event->type() == LuxUI::Event::FocusOut || event->type() == LuxUI::Event::Hide ||
        event->type() == LuxUI::Event::WindowDeactivate)
        ClearInput();
    return LuxUI::Widget::event(event);
}

void GameViewport::mousePressEvent(LuxUI::MouseEvent* event) {
    setFocus(LuxUI::Flags::MouseFocusReason);
    if (GetScene() && inputEnabled_ && !paused_) {
        LuxEngine::Input::SetExternalMousePosition(static_cast<double>(event->position().x()),
                                                   static_cast<double>(event->position().y()));
        if (const auto button = MouseButton(event->button()))
            LuxEngine::Input::SetExternalMouseButton(*button, true);
    }
    event->accept();
}

void GameViewport::mouseReleaseEvent(LuxUI::MouseEvent* event) {
    if (GetScene() && inputEnabled_ && !paused_ && hasFocus()) {
        if (const auto button = MouseButton(event->button()))
            LuxEngine::Input::SetExternalMouseButton(*button, false);
    }
    event->accept();
}

void GameViewport::mouseMoveEvent(LuxUI::MouseEvent* event) {
    if (GetScene() && inputEnabled_ && !paused_ && hasFocus())
        LuxEngine::Input::SetExternalMousePosition(static_cast<double>(event->position().x()),
                                                   static_cast<double>(event->position().y()));
    event->accept();
}

void GameViewport::paintEvent(LuxUI::PaintEvent*) {
    // SDL/GPU initialization can dispatch native window messages; finish it before QPainter.
    const double scale = devicePixelRatioF();
    const int pixelWidth = std::clamp(static_cast<int>(std::lround(width() * scale)), 1, 16384);
    const int pixelHeight = std::clamp(static_cast<int>(std::lround(height() * scale)), 1, 16384);
    auto* scene = GetScene();
    const auto lifetime = scene ? scene->Lifetime() : std::weak_ptr<int>{};
    const bool rendered = scene && renderer_->RenderGame(*scene, pixelWidth, pixelHeight, frame_);
    // A host can replace its document while initializing a native GPU surface.
    if (lifetime.expired() || GetScene() != scene)
        scene = nullptr;
    LuxUI::Painter painter(this);
    if (!painter.isActive())
        return;
    painter.fillRect(rect(), LuxUI::Color(22, 24, 28));
    if (!scene) {
        painter.setPen(LuxUI::Color(165, 170, 180));
        painter.drawText(rect(), LuxUI::Flags::AlignCenter, LUX_UI_TEXT("미리 볼 씬이 없습니다."));
        return;
    }
    painter.fillRect(rect(), Color(scene->GetSettings().clearColor));
    if (rendered) {
        frame_.setDevicePixelRatio(scale);
        painter.drawImage(LuxUI::RectF(rect()), frame_);
    } else {
        PaintSoftware(painter, *scene);
    }
}

LuxUI::Image GameViewport::ReadSoftwareTexture(const std::string& path) {
    const auto revision = LuxEngine::AssetManager::GetTextureRevision(path);
    if (const auto found = softwareTextures_.find(path); found != softwareTextures_.end()) {
        if (found->second.revision == revision)
            return found->second.image;
        softwareTextureBytes_ -= static_cast<size_t>(found->second.image.sizeInBytes());
        softwareTextures_.erase(found);
    }
    int width = 0, height = 0;
    std::vector<LuxEngine::uint8> rgba;
    if (!LuxEngine::AssetManager::DecodeTextureCpu(path, width, height, rgba))
        return {};
    const auto image =
        LuxUI::Image(rgba.data(), width, height, LuxUI::Image::Format_RGBA8888).copy();
    constexpr size_t budget = 64 * 1024 * 1024;
    const auto bytes = static_cast<size_t>(image.sizeInBytes());
    if (bytes && bytes <= budget) {
        if (softwareTextures_.size() >= 256 || bytes > budget - softwareTextureBytes_) {
            softwareTextures_.clear();
            softwareTextureBytes_ = 0;
        }
        softwareTextures_.emplace(path, SoftwareTexture{revision, image});
        softwareTextureBytes_ += bytes;
    }
    return image;
}

void GameViewport::PaintSoftware(LuxUI::Painter& painter, LuxEngine::Scene& scene) {
    const auto camera = ResolveGameViewCamera(scene, width(), height());
    painter.setRenderHint(LuxUI::Painter::Antialiasing);
    painter.setPen(LuxUI::Flags::NoPen);
    if (camera.perspective) {
        struct Face {
            LuxUI::PolygonF points;
            LuxUI::Color color;
            double depth;
        };

        std::vector<Face> faces;
        constexpr std::array<LuxEngine::Vec3, 8> corners{{{-.5f, -.5f, -.5f},
                                                          {.5f, -.5f, -.5f},
                                                          {.5f, .5f, -.5f},
                                                          {-.5f, .5f, -.5f},
                                                          {-.5f, -.5f, .5f},
                                                          {.5f, -.5f, .5f},
                                                          {.5f, .5f, .5f},
                                                          {-.5f, .5f, .5f}}};
        constexpr std::array<std::array<int, 4>, 6> indices{{{{0, 3, 2, 1}},
                                                             {{4, 5, 6, 7}},
                                                             {{0, 1, 5, 4}},
                                                             {{3, 7, 6, 2}},
                                                             {{0, 4, 7, 3}},
                                                             {{1, 2, 6, 5}}}};
        for (auto entity : scene.GetEntities()) {
            const auto* mesh = entity.TryGetComponent<LuxEngine::MeshRendererComponent>();
            if (!mesh || !entity.HasComponent<LuxEngine::TransformComponent>())
                continue;
            const auto world = scene.GetWorldMatrix(entity.GetId());
            for (size_t side = 0; side < indices.size(); ++side) {
                Face face;
                face.depth = 0;
                face.color =
                    Color(mesh->material.albedoColor).darker(100 + static_cast<int>(side) * 8);
                for (const int index : indices[side]) {
                    const auto point = Transform(world, corners[index]);
                    const auto projected = ProjectViewportPoint(camera.camera3D, size(), point);
                    if (!projected) {
                        face.points.clear();
                        break;
                    }
                    face.points << *projected;
                    const auto delta = point - camera.camera3D.GetPosition();
                    face.depth += delta.Dot(delta);
                }
                if (face.points.size() == 4)
                    faces.push_back(std::move(face));
            }
        }
        std::stable_sort(faces.begin(), faces.end(), [](const auto& left, const auto& right) {
            return left.depth > right.depth;
        });
        for (const auto& face : faces) {
            painter.setBrush(face.color);
            painter.drawPolygon(face.points);
        }
    }
    const auto project = [&](const LuxEngine::Mat4& world, LuxEngine::Vec3 point) {
        return Project2D(camera.camera2D.GetViewProjectionMatrix(), size(),
                         Transform(world, point));
    };
    const auto exposure = scene.GetSettings().exposure;
    const auto exposed = [exposure](LuxEngine::Vec4 color) {
        return LuxEngine::Vec4{color.x * exposure, color.y * exposure, color.z * exposure, color.w};
    };
    const auto drawQuad = [&](const LuxEngine::Mat4& world, LuxEngine::Vec4 color) {
        LuxUI::PolygonF points;
        for (const auto point :
             {LuxEngine::Vec3{-.5f, -.5f, 0}, {.5f, -.5f, 0}, {.5f, .5f, 0}, {-.5f, .5f, 0}})
            points << project(world, point);
        painter.setBrush(Color(color));
        painter.drawPolygon(points);
    };
    const std::array<LuxEngine::Vec4, 6> tileColors{{{},
                                                     {.36f, .65f, .27f, 1},
                                                     {.43f, .31f, .21f, 1},
                                                     {.48f, .51f, .54f, 1},
                                                     {.22f, .46f, .74f, .82f},
                                                     {.76f, .66f, .42f, 1}}};
    const auto entities = scene.GetEntitiesIn2DLayerOrder();
    for (auto entity : entities) {
        const auto* map = entity.TryGetComponent<LuxEngine::TilemapComponent>();
        if (!map)
            continue;
        const auto world = scene.GetWorldMatrix(entity.GetId());
        for (LuxEngine::uint32 y = 0; y < map->height; ++y)
            for (LuxEngine::uint32 x = 0; x < map->width; ++x) {
                const auto tile = map->Get(x, y);
                if (tile == 0 || tile >= tileColors.size())
                    continue;
                drawQuad(world *
                             LuxEngine::Mat4::Translate(
                                 {(x + .5f) * map->tileSize, (y + .5f) * map->tileSize, 0}) *
                             LuxEngine::Mat4::Scale({map->tileSize, map->tileSize, 1}),
                         exposed(tileColors[tile]));
            }
    }
    for (auto entity : entities) {
        const auto* sprite = entity.TryGetComponent<LuxEngine::SpriteRendererComponent>();
        if (!sprite)
            continue;
        const auto world = scene.GetWorldMatrix(entity.GetId());
        const auto color = exposed(sprite->color);
        if (!std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) ||
            !std::isfinite(color.w))
            continue;
        if (sprite->texturePath.empty()) {
            drawQuad(world, color);
            continue;
        }
        if (!sprite->columns || !sprite->rows || sprite->columns > 4096 || sprite->rows > 4096 ||
            sprite->frame >= sprite->columns * sprite->rows)
            continue;
        auto texture = ReadSoftwareTexture(scene.ResolveAssetPath(sprite->texturePath));
        if (texture.isNull())
            continue;
        const auto [left, right] =
            SpriteCell(texture.width(), sprite->frame % sprite->columns, sprite->columns);
        const auto [top, bottom] =
            SpriteCell(texture.height(), sprite->frame / sprite->columns, sprite->rows);
        LuxUI::Rect source(left, top, right - left, bottom - top);
        const std::array<float, 4> tint{color.x, color.y, color.z, color.w};
        if (!std::ranges::all_of(tint, [](float value) { return value == 1; })) {
            texture = texture.copy(source);
            TintSprite(texture, tint);
            source = texture.rect();
        }
        const auto topLeft = project(world, {-.5, .5, 0});
        const auto across = project(world, {.5, .5, 0}) - topLeft;
        const auto down = project(world, {-.5, -.5, 0}) - topLeft;
        if (!std::isfinite(topLeft.x()) || !std::isfinite(topLeft.y()) ||
            !std::isfinite(across.x()) || !std::isfinite(across.y()) || !std::isfinite(down.x()) ||
            !std::isfinite(down.y()))
            continue;
        painter.save();
        painter.setWorldTransform(
            LuxUI::Transform(across.x(), across.y(), down.x(), down.y(), topLeft.x(), topLeft.y()),
            true);
        painter.drawImage(LuxUI::RectF(0, 0, 1, 1), texture, LuxUI::RectF(source));
        painter.restore();
    }
}
} // namespace LuxEditor
