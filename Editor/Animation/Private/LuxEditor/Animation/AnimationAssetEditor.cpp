#include <LuxEditor/Animation/AnimationAssetEditor.hpp>
#include <LuxEditor/Scene/EditorDocument.hpp>
#include <LuxEditor/Viewport/SceneViewport2D.hpp>
#include <LuxEditor/Viewport/SceneViewport3D.hpp>
#include <LuxEngine/Assets/ModelAsset.hpp>
#include <LuxEngine/Scene/Entity.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
#include <sstream>

namespace LuxEditor {
namespace {
using namespace LuxEngine;
const char* Properties[] = {"위치",     "회전", "크기", "색상", "투명도", "스프라이트 프레임",
                            "실수 속성"};

LuxUI::String Text(const std::string& value) {
    return LuxUI::String::fromUtf8(value.c_str());
}

std::string Utf8(const LuxUI::String& value) {
    return value.toUtf8().toStdString();
}

class CurveCanvas final : public LuxUI::Widget {
public:
    AnimationAssetDocument& document;
    int track = 0, axis = 0;
    double time = 0, zoom = 100, scroll = 0;
    bool curves = true;
    std::set<size_t> selected;
    std::function<void(double)> seek;
    std::function<void()> changed;

    CurveCanvas(LuxUI::Widget* parent, AnimationAssetDocument& value)
        : LuxUI::Widget(parent), document(value) {
        setFocusPolicy(LuxUI::Flags::StrongFocus);
        setMinimumHeight(180);
        setObjectName(LUX_UI_TEXT("animationAssetCurve"));
    }

private:
    LuxUI::PointF last;
    bool dragging = false;
    int tangent = 0;
    double lower = -1, upper = 1;

    const AnimationTrack* Track() const {
        const auto& tracks = document.State().clip.tracks;
        return track >= 0 && size_t(track) < tracks.size() ? &tracks[track] : nullptr;
    }

    double X(double seconds) const {
        return 56 + (seconds - scroll) * zoom;
    }

    double Y(double value) const {
        return height() - 24 - (value - lower) / (upper - lower) * std::max(1, height() - 55);
    }

    double Value(double y) const {
        return lower + (height() - 24 - y) / std::max(1, height() - 55) * (upper - lower);
    }

    double At(double x) const {
        return std::clamp((x - 56) / zoom + scroll, 0.0, document.State().clip.duration);
    }

    double Snap(double seconds) const {
        const double fps = document.State().clip.framesPerSecond;
        return std::round(seconds * fps) / fps;
    }

    void paintEvent(LuxUI::PaintEvent*) override {
        LuxUI::Painter painter(this);
        painter.fillRect(rect(), LuxUI::Color(29, 31, 35));
        painter.setRenderHint(LuxUI::Painter::Antialiasing);
        const auto* source = Track();
        lower = -1;
        upper = 1;
        if (source && !source->keys.empty()) {
            lower = upper = source->keys.front().value[axis];
            for (const auto& key : source->keys) {
                lower = std::min(lower, key.value[axis]);
                upper = std::max(upper, key.value[axis]);
            }
            const double margin = std::max(.5, (upper - lower) * .2);
            lower -= margin;
            upper += margin;
        }
        const double step = zoom > 200 ? .1 : zoom > 60 ? .5 : 1;
        painter.setPen(LuxUI::Color(54, 57, 64));
        for (double t = std::floor(scroll / step) * step; X(t) < width(); t += step) {
            painter.drawLine(LuxUI::PointF(X(t), 24), LuxUI::PointF(X(t), height()));
            painter.drawText(LuxUI::PointF(X(t) + 3, 17),
                             LuxUI::String::number(t, 'f', step < 1 ? 1 : 0));
        }
        if (source) {
            if (curves && !source->keys.empty()) {
                LuxUI::PainterPath path;
                bool first = true;
                for (int x = 56; x < width(); ++x) {
                    auto sample = source->Sample(At(x));
                    const LuxUI::PointF point(x, Y(sample[axis]));
                    if (first)
                        path.moveTo(point);
                    else
                        path.lineTo(point);
                    first = false;
                }
                painter.setPen(LuxUI::Pen(LuxUI::Color(92, 181, 239), 2));
                painter.drawPath(path);
            }
            for (size_t i = 0; i < source->keys.size(); ++i) {
                const auto& key = source->keys[i];
                const double y = curves ? Y(key.value[axis]) : height() * .5;
                painter.setBrush(selected.contains(i) ? LuxUI::Color(255, 184, 79)
                                                      : LuxUI::Color(135, 172, 203));
                painter.setPen(LuxUI::Flags::NoPen);
                painter.drawEllipse(LuxUI::PointF(X(key.time), y), 5, 5);
                if (curves && selected.contains(i) &&
                    key.interpolation == AnimationInterpolation::Cubic) {
                    const double span = std::min(.25, document.State().clip.duration * .1);
                    painter.setPen(LuxUI::Color(172, 151, 94));
                    for (int direction : {-1, 1}) {
                        const double slope =
                            direction < 0 ? key.inTangent[axis] : key.outTangent[axis];
                        LuxUI::PointF end(X(key.time + direction * span),
                                          Y(key.value[axis] + direction * span * slope));
                        painter.drawLine(LuxUI::PointF(X(key.time), y), end);
                        painter.drawEllipse(end, 3, 3);
                    }
                }
            }
        }
        painter.setPen(LuxUI::Pen(LuxUI::Color(235, 106, 80), 1));
        painter.drawLine(LuxUI::PointF(X(time), 0), LuxUI::PointF(X(time), height()));
    }

    void mousePressEvent(LuxUI::MouseEvent* event) override {
        if (event->button() != LuxUI::Flags::LeftButton)
            return;
        setFocus();
        last = event->position();
        const auto* source = Track();
        tangent = 0;
        if (source) {
            if (curves && selected.size() == 1) {
                const auto& key = source->keys[*selected.begin()];
                const double span = std::min(.25, document.State().clip.duration * .1);
                for (int direction : {-1, 1}) {
                    const double slope = direction < 0 ? key.inTangent[axis] : key.outTangent[axis];
                    if (LuxUI::LineF(last, {X(key.time + direction * span),
                                            Y(key.value[axis] + direction * span * slope)})
                            .length() < 9) {
                        document.BeginEdit();
                        dragging = true;
                        tangent = direction;
                        return;
                    }
                }
            }
            for (size_t i = 0; i < source->keys.size(); ++i) {
                if (LuxUI::LineF(last, {X(source->keys[i].time),
                                        curves ? Y(source->keys[i].value[axis]) : height() * .5})
                        .length() < 9) {
                    if (event->modifiers() & LuxUI::Flags::ControlModifier) {
                        if (selected.contains(i))
                            selected.erase(i);
                        else
                            selected.insert(i);
                    } else if (!selected.contains(i)) {
                        selected.clear();
                        selected.insert(i);
                    }
                    document.BeginEdit();
                    dragging = true;
                    update();
                    if (changed)
                        changed();
                    return;
                }
            }
        }
        selected.clear();
        time = At(last.x());
        if (seek)
            seek(time);
        update();
    }

    void mouseMoveEvent(LuxUI::MouseEvent* event) override {
        if (!dragging) {
            if (event->buttons() & LuxUI::Flags::LeftButton) {
                time = At(event->position().x());
                if (seek)
                    seek(time);
                update();
            }
            return;
        }
        auto& keys = document.BeginEdit().clip.tracks[track].keys;
        if (tangent && selected.size() == 1) {
            auto& key = keys[*selected.begin()];
            const double seconds = (event->position().x() - X(key.time)) / zoom;
            if (std::abs(seconds) > 1e-6) {
                auto& values = tangent < 0 ? key.inTangent : key.outTangent;
                values[axis] = (Value(event->position().y()) - key.value[axis]) / seconds;
            }
        } else {
            double delta = Snap((event->position().x() - last.x()) / zoom), minDelta = -INFINITY,
                   maxDelta = INFINITY;
            for (auto index : selected) {
                minDelta = std::max(minDelta, -keys[index].time);
                maxDelta = std::min(maxDelta, document.State().clip.duration - keys[index].time);
                if (index && !selected.contains(index - 1))
                    minDelta = std::max(minDelta, keys[index - 1].time - keys[index].time +
                                                      1 / document.State().clip.framesPerSecond);
                if (index + 1 < keys.size() && !selected.contains(index + 1))
                    maxDelta = std::min(maxDelta, keys[index + 1].time - keys[index].time -
                                                      1 / document.State().clip.framesPerSecond);
            }
            if (minDelta <= maxDelta)
                delta = std::clamp(delta, minDelta, maxDelta);
            else
                delta = 0;
            const double value = curves ? Value(event->position().y()) - Value(last.y()) : 0;
            for (auto index : selected) {
                keys[index].time += delta;
                keys[index].value[axis] += value;
            }
            if (delta != 0)
                last.setX(last.x() + delta * zoom);
            last.setY(event->position().y());
        }
        update();
        if (seek)
            seek(time);
    }

    void mouseReleaseEvent(LuxUI::MouseEvent*) override {
        if (dragging) {
            dragging = false;
            document.CommitEdit();
            if (changed)
                changed();
        }
    }

    void wheelEvent(LuxUI::WheelEvent* event) override {
        if (event->modifiers() & LuxUI::Flags::ControlModifier)
            zoom = std::clamp(zoom * std::pow(1.2, event->angleDelta().y() / 120.), 10., 2000.);
        else
            scroll = std::max(0., scroll - event->angleDelta().y() / zoom);
        update();
        event->accept();
    }

    void keyPressEvent(LuxUI::KeyEvent* event) override {
        auto* source = Track();
        if (!source) {
            event->ignore();
            return;
        }
        if (event->key() == LuxUI::Flags::Key_Escape) {
            document.CancelEdit();
            dragging = false;
            if (changed)
                changed();
            event->accept();
            return;
        }
        if (event->matches(LuxUI::KeySequence::SelectAll)) {
            for (size_t i = 0; i < source->keys.size(); ++i)
                selected.insert(i);
            update();
            event->accept();
            return;
        }
        if (event->matches(LuxUI::KeySequence::Copy) || event->matches(LuxUI::KeySequence::Cut)) {
            std::ostringstream text;
            text.precision(17);
            text << "LuxAnimationKeys1\n";
            for (auto index : selected) {
                const auto& key = source->keys[index];
                text << key.time << ' ' << int(key.interpolation);
                for (auto value : {key.value, key.inTangent, key.outTangent})
                    for (double v : value)
                        text << ' ' << v;
                text << '\n';
            }
            LuxUI::Application::clipboard()->setText(Text(text.str()));
            if (event->matches(LuxUI::KeySequence::Copy)) {
                event->accept();
                return;
            }
        }
        if (event->key() == LuxUI::Flags::Key_Delete || event->matches(LuxUI::KeySequence::Cut)) {
            document.Edit([&](auto& state) {
                auto& keys = state.clip.tracks[track].keys;
                for (auto at = selected.rbegin(); at != selected.rend(); ++at)
                    if (*at < keys.size())
                        keys.erase(keys.begin() + *at);
            });
            selected.clear();
            if (changed)
                changed();
            event->accept();
            return;
        }
        if (event->matches(LuxUI::KeySequence::Paste)) {
            std::istringstream text(Utf8(LuxUI::Application::clipboard()->text()));
            std::string magic;
            std::getline(text, magic);
            if (magic != "LuxAnimationKeys1") {
                event->accept();
                return;
            }
            std::vector<AnimationKey> keys;
            double first = -1;
            for (size_t count = 0; count < 100000; ++count) {
                AnimationKey key;
                int interpolation;
                if (!(text >> key.time >> interpolation))
                    break;
                if (interpolation < 0 || interpolation > 2)
                    break;
                key.interpolation = AnimationInterpolation(interpolation);
                for (auto* values : {&key.value, &key.inTangent, &key.outTangent})
                    for (double& v : *values)
                        text >> v;
                if (!text)
                    break;
                if (first < 0)
                    first = key.time;
                key.time = Snap(time + key.time - first);
                if (key.time >= 0 && key.time <= document.State().clip.duration)
                    keys.push_back(key);
            }
            try {
                document.Edit([&](auto& state) {
                    for (const auto& key : keys)
                        state.clip.tracks[track].SetKey(key);
                });
            } catch (const std::exception&) {
            }
            if (changed)
                changed();
            event->accept();
            return;
        }
        event->ignore();
    }
};

class AnimatorCanvas final : public LuxUI::Widget {
public:
    AnimationAssetDocument& document;
    size_t layer = 0, selected = 0;
    std::function<void()> changed;

    AnimatorCanvas(LuxUI::Widget* parent, AnimationAssetDocument& value)
        : LuxUI::Widget(parent), document(value) {
        setMinimumSize(400, 200);
        setObjectName(LUX_UI_TEXT("animationAnimatorGraph"));
    }

private:
    std::optional<size_t> drag, connection;
    LuxUI::PointF start, offset;

    LuxUI::RectF Box(size_t i) const {
        const auto& state = document.State().controller.layers[layer].states[i];
        auto p = state.editorPosition;
        if (p.x == 0 && p.y == 0)
            p = {40. + double(i % 3) * 190., 40. + double(i / 3) * 100.};
        return {p.x, p.y, 160, 58};
    }

    std::optional<size_t> Hit(LuxUI::PointF p) const {
        const auto& states = document.State().controller.layers[layer].states;
        for (size_t i = 0; i < states.size(); ++i)
            if (Box(i).contains(p))
                return i;
        return {};
    }

    void paintEvent(LuxUI::PaintEvent*) override {
        LuxUI::Painter painter(this);
        painter.fillRect(rect(), LuxUI::Color(30, 32, 36));
        if (layer >= document.State().controller.layers.size())
            return;
        const auto& data = document.State().controller.layers[layer];
        painter.setRenderHint(LuxUI::Painter::Antialiasing);
        painter.setPen(LuxUI::Color(145, 152, 164));
        for (const auto& transition : data.transitions) {
            auto from = std::find_if(data.states.begin(), data.states.end(),
                                     [&](const auto& s) { return s.name == transition.from; }),
                 to = std::find_if(data.states.begin(), data.states.end(),
                                   [&](const auto& s) { return s.name == transition.to; });
            if (to == data.states.end())
                continue;
            auto a = from == data.states.end() ? LuxUI::PointF(10, 20)
                                               : Box(size_t(from - data.states.begin())).center(),
                 b = Box(size_t(to - data.states.begin())).center();
            painter.drawLine(a, b);
            auto delta = b - a;
            double length = std::hypot(delta.x(), delta.y());
            if (length > 1) {
                auto head = a + delta * .6;
                auto unit = delta / length;
                painter.drawLine(head, head - unit * 10 + LuxUI::PointF(-unit.y(), unit.x()) * 5);
                painter.drawLine(head, head - unit * 10 - LuxUI::PointF(-unit.y(), unit.x()) * 5);
            }
        }
        for (size_t i = 0; i < data.states.size(); ++i) {
            auto box = Box(i);
            painter.setPen(LuxUI::Pen(
                i == selected ? LuxUI::Color(230, 171, 73) : LuxUI::Color(88, 94, 105), 2));
            painter.setBrush(data.states[i].name == data.entry ? LuxUI::Color(54, 88, 69)
                                                               : LuxUI::Color(47, 52, 61));
            painter.drawRoundedRect(box, 5, 5);
            painter.drawText(box.adjusted(8, 3, -8, -25), Text(data.states[i].name));
            painter.setPen(LuxUI::Color(170, 180, 193));
            painter.drawText(box.adjusted(8, 29, -8, -4),
                             Text(std::filesystem::path(data.states[i].clip).filename().string()));
        }
    }

    void mousePressEvent(LuxUI::MouseEvent* event) override {
        if (layer >= document.State().controller.layers.size())
            return;
        auto hit = Hit(event->position());
        if (!hit)
            return;
        selected = *hit;
        start = event->position();
        if (event->modifiers() & LuxUI::Flags::ShiftModifier)
            connection = hit;
        else {
            drag = hit;
            offset = Box(*hit).topLeft() - start;
            document.BeginEdit();
        }
        if (changed)
            changed();
        update();
    }

    void mouseMoveEvent(LuxUI::MouseEvent* event) override {
        if (drag) {
            auto point = event->position() + offset;
            document.BeginEdit().controller.layers[layer].states[*drag].editorPosition = {
                std::max(0., point.x()), std::max(0., point.y())};
            update();
        }
    }

    void mouseReleaseEvent(LuxUI::MouseEvent* event) override {
        if (drag) {
            drag.reset();
            document.CommitEdit();
        }
        if (connection) {
            auto to = Hit(event->position());
            if (to && *to != *connection) {
                auto from = *connection;
                document.Edit([&](auto& state) {
                    auto& data = state.controller.layers[layer];
                    data.transitions.push_back(
                        {data.states[from].name, data.states[*to].name, .2, -1, {}});
                });
            }
            connection.reset();
        }
        if (changed)
            changed();
        update();
    }
};
} // namespace

struct AnimationAssetEditor::Impl {
    AnimationAssetEditor& owner;
    EditorDocument& source;
    EditorDocument preview;
    AnimationAssetDocument document;
    std::function<void(const LuxUI::String&, const std::function<void()>&)> recordedEdit;
    std::string sourceSnapshot;
    uint32 target = 0;
    AnimationPlayback playback;
    bool refreshing = false;
    LuxUI::Label* status = nullptr;
    LuxUI::ListWidget* tracks = nullptr;
    LuxUI::ListWidget* keys = nullptr;
    LuxUI::TreeWidget* bones = nullptr;
    LuxUI::TabWidget* pages = nullptr;
    SceneViewport2D* view2D = nullptr;
    SceneViewport3D* view3D = nullptr;
    CurveCanvas* curve = nullptr;
    AnimatorCanvas* graph = nullptr;
    LuxUI::DoubleSpinBox *sheetColumns = nullptr, *sheetRows = nullptr;
    LuxUI::ComboBox* fbxClip = nullptr;
    std::string shownModel;
    LuxUI::DoubleSpinBox *time = nullptr, *duration = nullptr, *fps = nullptr, *speed = nullptr,
                         *keyTime = nullptr, *inTangent = nullptr, *outTangent = nullptr;
    LuxUI::DoubleSpinBox* values[4]{};
    LuxUI::ComboBox *property = nullptr, *wrap = nullptr, *interpolation = nullptr,
                    *layers = nullptr;
    LuxUI::LineEdit *binding = nullptr, *field = nullptr, *eventName = nullptr,
                    *eventPayload = nullptr;
    LuxUI::Action* autoKey = nullptr;
    LuxUI::Action* rootMotion = nullptr;
    LuxUI::Timer* timer = nullptr;
    LuxUI::ElapsedTimer clock;
    Ref<AnimatorPlayer> animator;
    std::vector<AnimationEvent> fired;

    Impl(AnimationAssetEditor& widget, EditorDocument& scene, decltype(recordedEdit) edit)
        : owner(widget), source(scene), recordedEdit(std::move(edit)) {}

    LuxUI::PushButton* Button(LuxUI::Layout* layout, const char* label,
                              const std::function<void()>& action) {
        auto* button = new LuxUI::PushButton(LuxUI::String::fromUtf8(label), &owner);
        layout->addWidget(button);
        LuxUI::Object::connect(button, &LuxUI::PushButton::clicked, &owner, action);
        return button;
    }

    LuxUI::Action* Button(LuxUI::ToolBar* toolbar, const char* label,
                          const std::function<void()>& action) {
        auto* command = toolbar->addAction(LuxUI::String::fromUtf8(label));
        LuxUI::Object::connect(command, &LuxUI::Action::triggered, &owner, action);
        return command;
    }

    LuxUI::DoubleSpinBox* Spin(LuxUI::FormLayout* form, const char* label, double low,
                               double high) {
        auto* spin = new LuxUI::DoubleSpinBox(&owner);
        spin->setRange(low, high);
        spin->setDecimals(6);
        form->addRow(LuxUI::String::fromUtf8(label), spin);
        return spin;
    }

    void Error(const std::string& message) {
        status->setText(Text(message));
    }

    void ResetPreview() {
        playback.playing = false;
        animator.reset();
        target = 0;
        auto entity = source.GetSelected();
        if (!source.GetScene().SerializeToString(sourceSnapshot))
            return;
        if (!entity) {
            LuxEngine::Scene empty;
            std::string data;
            empty.SerializeToString(data);
            preview.GetScene().DeserializeFromString(data);
            auto root = preview.CreateEntity("미리보기");
            target = root.GetId();
            root.AddComponent<MeshRendererComponent>();
            root.AddComponent<SpriteRendererComponent>();
            preview.Select(target);
        }

        if (entity && !preview.GetScene().DeserializeFromString(sourceSnapshot))
            return;
        preview.GetScene().SetSourcePath(source.GetScene().GetSourcePath());
        if (entity)
            target = entity.GetId();
        preview.Select(target);
        preview.GetScene().SetParent(target, 0, false);
        std::set<uint32> keep;
        std::vector<uint32> pending{target};
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            keep.insert(id);
            const auto children = preview.GetScene().GetChildren(id);
            pending.insert(pending.end(), children.begin(), children.end());
        }
        for (auto other : preview.GetScene().GetEntities())
            if (!keep.contains(other.GetId()) && preview.GetScene().IsAlive(other.GetId()))
                preview.GetScene().DestroyEntity(other);
        if (auto* transform = preview.GetScene().TryGetComponent<TransformComponent>(target))
            transform->position = {};
        if (!document.State().isController && !document.State().clip.modelPath.empty()) {
            std::string error;
            auto model = ModelAsset::LoadFbx(document.State().clip.modelPath, error);
            bool bound = false;
            for (auto item : preview.GetScene().GetEntities())
                if (const auto* animation = item.TryGetComponent<ModelAnimationComponent>();
                    animation && preview.GetScene().ResolveAssetPath(animation->modelPath) ==
                                     document.State().clip.modelPath)
                    bound = true;
            if (model && !bound) {
                auto& scene = preview.GetScene();
                if (scene.HasComponent<MeshRendererComponent>(target))
                    scene.RemoveComponent<MeshRendererComponent>(target);
                if (scene.HasComponent<SpriteRendererComponent>(target))
                    scene.RemoveComponent<SpriteRendererComponent>(target);
                std::vector<uint32> nodes;
                for (const auto& node : model->nodes) {
                    auto created = scene.CreateEntity(node.name);
                    auto& transform = created.GetComponent<TransformComponent>();
                    transform = {node.localTransform.position, node.localTransform.rotation,
                                 node.localTransform.scale};
                    nodes.push_back(created.GetId());
                }
                for (size_t index = 0; index < nodes.size(); ++index) {
                    scene.SetParent(nodes[index],
                                    model->nodes[index].parent < 0
                                        ? target
                                        : nodes[size_t(model->nodes[index].parent)],
                                    false);
                    for (auto submesh : model->nodes[index].submeshes) {
                        auto part = scene.CreateEntity(model->submeshes[submesh].name);
                        scene.SetParent(part.GetId(), nodes[index], false);
                        auto& mesh = part.AddComponent<MeshRendererComponent>();
                        mesh.meshPath = document.State().clip.modelPath;
                        mesh.modelSubmesh = int32(submesh);
                        auto& animation = part.AddComponent<ModelAnimationComponent>();
                        animation.modelPath = mesh.meshPath;
                        animation.nodeIndex = uint32(index);
                        animation.applyNodeTransform = false;
                    }
                }
                // Node transforms and mesh deformation share the same evaluated pose.
                for (size_t index = 0; index < nodes.size(); ++index) {
                    auto& animation = scene.AddComponent<ModelAnimationComponent>(nodes[index]);
                    animation.modelPath = document.State().clip.modelPath;
                    animation.nodeIndex = uint32(index);
                    animation.applyNodeTransform = true;
                }
            }
        }
        if (document.State().isController) {
            auto controller = CreateRef<AnimatorController>(document.State().controller);
            controller->sourcePath = document.Path();
            std::string error;
            animator = CreateRef<AnimatorPlayer>();
            if (!animator->SetController(controller, error)) {
                Error(error);
                animator.reset();
            }
        }
        view2D->FocusSelection();
        view3D->FocusSelection();
    }

    void Refresh() {
        refreshing = true;
        const auto& state = document.State();
        tracks->clear();
        if (!state.isController) {
            for (const auto& track : state.clip.tracks)
                tracks->addItem(Text(track.target) + LUX_UI_TEXT(" / ") +
                                LuxUI::String::fromUtf8(Properties[int(track.property)]));
            duration->setValue(state.clip.duration);
            fps->setValue(state.clip.framesPerSecond);
            wrap->setCurrentIndex(int(state.clip.GetWrap()));
            time->setMaximum(state.clip.duration);
            if (sheetColumns)
                sheetColumns->setValue(state.clip.spriteColumns);
            if (sheetRows)
                sheetRows->setValue(state.clip.spriteRows);

        } else {
            time->setMaximum(86400);
            for (const auto& layer : state.controller.layers)
                tracks->addItem(Text(layer.name));
        }
        layers->clear();
        for (const auto& layer : state.controller.layers)
            layers->addItem(Text(layer.name));
        if (state.isController && graph->layer < state.controller.layers.size())
            layers->setCurrentIndex(int(graph->layer));
        if (!state.isController && curve->track >= 0 &&
            size_t(curve->track) < state.clip.tracks.size()) {
            tracks->setCurrentRow(curve->track);
            const auto& track = state.clip.tracks[curve->track];
            binding->setText(Text(track.target));
            field->setText(Text(track.field));
            property->setCurrentIndex(int(track.property));
            keys->clear();
            for (const auto& key : track.keys)
                keys->addItem(LuxUI::String::number(key.time, 'f', 4));
            if (!curve->selected.empty() && *curve->selected.begin() < track.keys.size()) {
                const auto index = *curve->selected.begin();
                const auto& key = track.keys[index];
                keys->setCurrentRow(int(index));
                keyTime->setValue(key.time);
                for (size_t i = 0; i < 4; ++i)
                    values[i]->setValue(track.property == AnimationProperty::Rotation
                                            ? key.value[i] * 180 / std::numbers::pi
                                            : key.value[i]);
                interpolation->setCurrentIndex(int(key.interpolation));
                inTangent->setValue(key.inTangent[curve->axis]);
                outTangent->setValue(key.outTangent[curve->axis]);
            }
        }

        if (state.isController) {
            if (auto* list = owner.findChild<LuxUI::ListWidget*>(
                    LUX_UI_TEXT("animationControllerParameters"))) {
                list->clear();
                for (const auto& parameter : state.controller.parameters)
                    list->addItem(Text(parameter.name) + LUX_UI_TEXT(" = ") +
                                  LuxUI::String::number(parameter.value));
            }
            if (auto* list = owner.findChild<LuxUI::ListWidget*>(
                    LUX_UI_TEXT("animationControllerTransitions"))) {
                list->clear();
                if (graph->layer < state.controller.layers.size())
                    for (const auto& transition : state.controller.layers[graph->layer].transitions)
                        list->addItem(
                            Text(transition.from.empty() ? "Any" : transition.from) +
                            LUX_UI_TEXT(" → ") + Text(transition.to) + LUX_UI_TEXT("  (") +
                            LuxUI::String::number(transition.conditions.size()) + LUX_UI_TEXT(")"));
            }
        }

        if (!state.isController && fbxClip) {
            if (shownModel != state.clip.modelPath) {
                shownModel = state.clip.modelPath;
                fbxClip->clear();
                fbxClip->addItem(LUX_UI_TEXT("바인드 포즈"));
                bones->clear();
                if (!shownModel.empty()) {
                    std::string error;
                    auto model = ModelAsset::LoadFbx(shownModel, error);
                    if (model) {
                        for (const auto& clip : model->animations)
                            fbxClip->addItem(Text(clip.name) + LUX_UI_TEXT("  ") +
                                             LuxUI::String::number(clip.duration, 'f', 2) +
                                             LUX_UI_TEXT(" s"));
                        std::vector<LuxUI::TreeWidgetItem*> items(model->nodes.size());
                        for (size_t node = 0; node < items.size(); ++node)
                            items[node] =
                                new LuxUI::TreeWidgetItem({Text(model->nodes[node].name)});
                        for (size_t node = 0; node < items.size(); ++node) {
                            if (model->nodes[node].parent < 0)
                                bones->addTopLevelItem(items[node]);
                            else
                                items[size_t(model->nodes[node].parent)]->addChild(items[node]);
                        }
                        bones->expandAll();
                    }
                }
            }
            fbxClip->setCurrentIndex(state.clip.modelClip + 1);
        }
        graph->update();
        curve->update();
        refreshing = false;
    }

    void Changed() {
        const double cursor = playback.cursor;
        ResetPreview();
        Refresh();
        owner.Seek(cursor);
        view2D->FocusSelection();
        view3D->FocusSelection();
    }

    void AddTrack() {
        if (document.State().isController)
            return;
        document.Edit([&](auto& state) {
            AnimationTrack track;
            track.target = Utf8(binding->text());
            track.field = Utf8(field->text());
            track.property = AnimationProperty(property->currentIndex());
            if (std::any_of(
                    state.clip.tracks.begin(), state.clip.tracks.end(), [&](const auto& other) {
                        return other.target == track.target && other.property == track.property &&
                               other.field == track.field;
                    }))
                return;
            state.clip.tracks.push_back(track);
            curve->track = int(state.clip.tracks.size() - 1);
            curve->selected.clear();
        });
        Changed();
    }

    void AddKey() {
        if (document.State().isController || curve->track < 0 ||
            size_t(curve->track) >= document.State().clip.tracks.size())
            return;
        document.Edit([&](auto& state) {
            AnimationKey key;
            key.time = time->value();
            for (size_t i = 0; i < 4; ++i)
                key.value[i] =
                    state.clip.tracks[curve->track].property == AnimationProperty::Rotation
                        ? ToRadians(values[i]->value())
                        : values[i]->value();
            key.interpolation = AnimationInterpolation(interpolation->currentIndex());
            state.clip.tracks[curve->track].SetKey(key);
        });
        Changed();
    }

    void Transform(uint32 id, const TransformComponent& value) {
        if (auto* transform = preview.GetScene().TryGetComponent<TransformComponent>(id))
            *transform = value;
        if (!autoKey->isChecked() || document.State().isController || id != target)
            return;
        document.BeginEdit();
        auto& clip = document.BeginEdit().clip;
        const std::array<Vec3, 3> channels{value.position, value.rotation, value.scale};
        for (size_t i = 0; i < 3; ++i) {
            const auto property = AnimationProperty(i);
            auto track = std::find_if(clip.tracks.begin(), clip.tracks.end(), [&](const auto& t) {
                return t.target.empty() && t.property == property;
            });
            if (track == clip.tracks.end()) {
                clip.tracks.push_back({{}, property, {}, {}});
                track = std::prev(clip.tracks.end());
            }
            AnimationKey key;
            key.time = time->value();
            key.value = {channels[i].x, channels[i].y, channels[i].z, 0};
            track->SetKey(key);
        }
    }

    void Save() {
        auto path = document.Path();
        if (path.empty())
            path = Utf8(LuxUI::FileDialog::getSaveFileName(
                &owner, LUX_UI_TEXT("애니메이션 저장"), {},
                document.State().isController ? LUX_UI_TEXT("Animator (*.luxanimator)")
                                              : LUX_UI_TEXT("Animation (*.luxanim)")));
        if (!path.empty())
            owner.SaveAsset(path);
    }

    void ChooseModel() {
        auto path = Utf8(LuxUI::FileDialog::getOpenFileName(&owner, LUX_UI_TEXT("FBX 애니메이션"),
                                                            {}, LUX_UI_TEXT("FBX (*.fbx)")));
        if (path.empty())
            return;
        std::string error;
        auto model = ModelAsset::LoadFbx(path, error);
        if (!model) {
            Error(error);
            return;
        }
        bones->clear();
        std::vector<LuxUI::TreeWidgetItem*> items(model->nodes.size());
        for (size_t i = 0; i < items.size(); ++i) {
            items[i] = new LuxUI::TreeWidgetItem({Text(model->nodes[i].name)});
            if (model->nodes[i].parent >= 0) {
            } else
                bones->addTopLevelItem(items[i]);
        }
        for (size_t i = 0; i < items.size(); ++i)
            if (model->nodes[i].parent >= 0)
                items[model->nodes[i].parent]->addChild(items[i]);
        bones->expandAll();
        document.Edit([&](auto& state) {
            state.clip.modelPath = path;
            state.clip.modelClip = model->animations.empty() ? -1 : 0;
            if (!model->animations.empty())
                state.clip.duration = std::max(.001, model->animations[0].duration);
        });
        Changed();
    }

    void Assign() {
        if (document.Path().empty()) {
            Save();
            if (document.Path().empty())
                return;
        }
        auto entity = source.GetSelected();
        if (!entity)
            return;
        const auto id = entity.GetId();
        const auto path = document.Path();
        recordedEdit(LUX_UI_TEXT("애니메이션 에셋 연결"), [&, id, path] {
            Entity selected(id, &source.GetScene());
            if (document.State().isController) {
                auto* component = selected.TryGetComponent<AnimatorComponent>();
                if (!component)
                    component = &selected.AddComponent<AnimatorComponent>();
                component->controllerPath = path;
                component->rootMotion = rootMotion->isChecked();
                component->player.reset();
            } else {
                auto* component = selected.TryGetComponent<AnimationComponent>();
                if (!component)
                    component = &selected.AddComponent<AnimationComponent>();
                component->assetPath = path;
                component->rootMotion = rootMotion->isChecked();
                component->asset.reset();
                component->loadedPath.clear();
            }
        });
    }
};

AnimationAssetEditor::AnimationAssetEditor(
    LuxUI::Widget* parent, EditorDocument& scene,
    std::function<void(const LuxUI::String&, const std::function<void()>&)> edit)
    : LuxUI::Widget(parent), impl_(std::make_unique<Impl>(*this, scene, std::move(edit))) {
    setObjectName(LUX_UI_TEXT("animationAssetEditor"));
    setFocusPolicy(LuxUI::Flags::StrongFocus);
    auto& p = *impl_;
    auto* layout = new LuxUI::VBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    // A toolbar can overflow at Windows display scaling instead of widening the whole editor.
    auto* toolbar = new LuxUI::ToolBar(this);
    toolbar->setObjectName(LUX_UI_TEXT("animationAssetToolbar"));
    toolbar->setMinimumWidth(0);
    toolbar->setSizePolicy(LuxUI::SizePolicy::Ignored, LuxUI::SizePolicy::Preferred);
    layout->addWidget(toolbar);
    p.Button(toolbar, "새 클립", [&p] {
        if (!p.owner.ConfirmDiscard())
            return;
        p.document.New(false);
        p.curve->selected.clear();
        p.ResetPreview();
        p.Refresh();
    });
    p.Button(toolbar, "새 Animator", [&p] {
        if (!p.owner.ConfirmDiscard())
            return;
        p.document.New(true);
        p.graph->selected = 0;
        p.ResetPreview();
        p.Refresh();
    });
    p.Button(toolbar, "열기", [this] {
        if (!ConfirmDiscard())
            return;
        auto path = Utf8(
            LuxUI::FileDialog::getOpenFileName(this, LUX_UI_TEXT("애니메이션 에셋 열기"), {},
                                               LUX_UI_TEXT("Animation (*.luxanim *.luxanimator)")));
        if (!path.empty())
            OpenAsset(path);
    });
    p.Button(toolbar, "저장", [&p] { p.Save(); });
    p.Button(toolbar, "씬에 연결", [&p] { p.Assign(); });
    p.Button(toolbar, "실행 취소", [&p] {
        if (p.document.Undo())
            p.Changed();
    });
    p.Button(toolbar, "다시 실행", [&p] {
        if (p.document.Redo())
            p.Changed();
    });
    p.Button(toolbar, "재생 / 일시정지", [this] {
         Play(!impl_->playback.playing);
     })->setObjectName(LUX_UI_TEXT("assetAnimationPlay"));
    p.Button(toolbar, "정지", [this] {
        Play(false);
        impl_->ResetPreview();
        Seek(0);
    });
    p.autoKey = toolbar->addAction(LUX_UI_TEXT("자동 키"));
    p.autoKey->setCheckable(true);
    p.rootMotion = toolbar->addAction(LUX_UI_TEXT("루트 모션"));
    p.rootMotion->setCheckable(true);
    auto* split = new LuxUI::Splitter(LuxUI::Flags::Horizontal, this);
    layout->addWidget(split, 1);
    auto* left = new LuxUI::Widget(split);
    auto* leftLayout = new LuxUI::VBoxLayout(left);
    leftLayout->addWidget(new LuxUI::Label(LUX_UI_TEXT("트랙 / 레이어"), left));
    p.tracks = new LuxUI::ListWidget(left);
    p.tracks->setObjectName(LUX_UI_TEXT("animationAssetTracks"));
    leftLayout->addWidget(p.tracks, 1);
    p.bones = new LuxUI::TreeWidget(left);
    p.bones->setHeaderLabel(LUX_UI_TEXT("스켈레톤"));
    leftLayout->addWidget(p.bones, 1);
    p.Button(leftLayout, "FBX 클립 가져오기", [&p] { p.ChooseModel(); });
    p.Button(leftLayout, "스프라이트 시트", [&p] {
        auto path =
            Utf8(LuxUI::FileDialog::getOpenFileName(&p.owner, LUX_UI_TEXT("스프라이트 시트"), {},
                                                    LUX_UI_TEXT("Images (*.png *.jpg *.webp)")));
        if (!path.empty())
            p.document.Edit([&](auto& state) { state.clip.spriteSheet = path; });
        p.Changed();
    });
    p.Button(leftLayout, "프레임 클립 생성", [&p] {
        if (p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            auto& clip = state.clip;
            AnimationTrack track;
            track.property = AnimationProperty::SpriteFrame;
            const uint32 count = clip.spriteColumns * clip.spriteRows;
            for (uint32 i = 0; i < count; ++i) {
                AnimationKey key;
                key.time = i / clip.framesPerSecond;
                key.value[0] = i;
                key.interpolation = AnimationInterpolation::Step;
                track.SetKey(key);
            }
            std::erase_if(clip.tracks, [](const auto& value) {
                return value.target.empty() && value.property == AnimationProperty::SpriteFrame;
            });
            clip.duration = std::max(clip.duration, count / clip.framesPerSecond);
            clip.tracks.push_back(std::move(track));
            p.curve->track = int(clip.tracks.size() - 1);
        });
        p.Changed();
    });

    auto* center = new LuxUI::Splitter(LuxUI::Flags::Vertical, split);
    p.pages = new LuxUI::TabWidget(center);
    auto select = [&p](std::optional<uint32> id) { p.preview.Select(id); };
    auto transform = [&p](uint32 id, const TransformComponent& value) { p.Transform(id, value); };
    auto begin = [&p] {
        if (p.autoKey->isChecked())
            p.document.BeginEdit();
    };
    auto end = [&p] {
        if (p.autoKey->isChecked()) {
            p.document.CommitEdit();
            p.Changed();
        }
    };
    p.view2D = new SceneViewport2D(
        p.pages, p.preview, select, transform, [](uint32, uint32, uint32, uint8) {}, begin, end);
    p.view3D = new SceneViewport3D(p.pages, p.preview, select, transform, begin, end);
    p.view3D->SetGridVisible(false);
    p.pages->addTab(p.view3D, LUX_UI_TEXT("3D 미리보기"));
    p.pages->addTab(p.view2D, LUX_UI_TEXT("2D 미리보기"));
    p.graph = new AnimatorCanvas(p.pages, p.document);
    p.pages->addTab(p.graph, LUX_UI_TEXT("Animator"));
    p.graph->changed = [&p] { p.Refresh(); };
    auto* timeline = new LuxUI::Widget(center);
    auto* timelineLayout = new LuxUI::VBoxLayout(timeline);
    auto* timelineTools = new LuxUI::HBoxLayout;
    timelineLayout->addLayout(timelineTools);
    p.Button(timelineTools, "이전 프레임",
             [this] { Seek(Time() - 1 / impl_->document.State().clip.framesPerSecond); });
    p.Button(timelineTools, "다음 프레임",
             [this] { Seek(Time() + 1 / impl_->document.State().clip.framesPerSecond); });
    auto* mode = new LuxUI::ComboBox(timeline);
    mode->addItems({LUX_UI_TEXT("곡선"), LUX_UI_TEXT("도프 시트")});
    timelineTools->addWidget(mode);
    auto* axis = new LuxUI::ComboBox(timeline);
    axis->addItems(
        {LUX_UI_TEXT("X / R"), LUX_UI_TEXT("Y / G"), LUX_UI_TEXT("Z / B"), LUX_UI_TEXT("W / A")});
    timelineTools->addWidget(axis);
    p.curve = new CurveCanvas(timeline, p.document);
    p.curve->seek = [this](double value) { Seek(value); };
    p.curve->changed = [&p] { p.Refresh(); };
    timelineLayout->addWidget(p.curve, 1);
    connect(mode, &LuxUI::ComboBox::currentIndexChanged, this, [&p](int value) {
        p.curve->curves = value == 0;
        p.curve->update();
    });
    connect(axis, &LuxUI::ComboBox::currentIndexChanged, this, [&p](int value) {
        p.curve->axis = value;
        p.Refresh();
    });
    auto* right = new LuxUI::TabWidget(split);
    auto* clipPage = new LuxUI::Widget(right);
    auto* clipForm = new LuxUI::FormLayout(clipPage);
    p.time = p.Spin(clipForm, "시간", 0, 86400);
    p.time->setObjectName(LUX_UI_TEXT("animationAssetTime"));
    p.duration = p.Spin(clipForm, "길이", .001, 86400);
    p.fps = p.Spin(clipForm, "FPS", 1, 1000);
    p.speed = p.Spin(clipForm, "속도", -100, 100);
    p.speed->setValue(1);
    p.wrap = new LuxUI::ComboBox(clipPage);
    p.wrap->addItems({LUX_UI_TEXT("한 번"), LUX_UI_TEXT("반복"), LUX_UI_TEXT("왕복")});
    clipForm->addRow(LUX_UI_TEXT("재생 방식"), p.wrap);
    auto* rootTarget = new LuxUI::LineEdit(clipPage);
    rootTarget->setPlaceholderText(LUX_UI_TEXT("기본 루트 또는 본 이름"));
    clipForm->addRow(LUX_UI_TEXT("루트 모션 본"), rootTarget);
    connect(rootTarget, &LuxUI::LineEdit::editingFinished, this, [&p, rootTarget] {
        p.document.Edit(
            [&](auto& state) { state.clip.rootMotionTarget = Utf8(rootTarget->text()); });
        p.Changed();
    });
    auto* columns = p.Spin(clipForm, "시트 열", 1, 4096);
    p.sheetColumns = columns;
    columns->setValue(1);
    auto* rows = p.Spin(clipForm, "시트 행", 1, 4096);
    p.sheetRows = rows;
    rows->setValue(1);
    auto* modelClip = new LuxUI::ComboBox(clipPage);
    modelClip->addItem(LUX_UI_TEXT("바인드 포즈"));
    p.fbxClip = modelClip;
    clipForm->addRow(LUX_UI_TEXT("FBX 클립"), modelClip);
    right->addTab(clipPage, LUX_UI_TEXT("클립"));
    auto* keyPage = new LuxUI::Widget(right);
    auto* keyLayout = new LuxUI::VBoxLayout(keyPage);
    auto* keyForm = new LuxUI::FormLayout;
    keyLayout->addLayout(keyForm);
    p.binding = new LuxUI::LineEdit(keyPage);
    p.binding->setPlaceholderText(LUX_UI_TEXT("소유자 또는 자식/경로"));
    keyForm->addRow(LUX_UI_TEXT("대상"), p.binding);
    p.field = new LuxUI::LineEdit(keyPage);
    keyForm->addRow(LUX_UI_TEXT("스크립트 필드"), p.field);
    p.property = new LuxUI::ComboBox(keyPage);
    for (auto name : Properties)
        p.property->addItem(LuxUI::String::fromUtf8(name));
    keyForm->addRow(LUX_UI_TEXT("속성"), p.property);
    p.Button(keyLayout, "트랙 추가", [&p] { p.AddTrack(); });
    p.Button(keyLayout, "트랙 삭제", [&p] {
        if (p.document.State().isController || p.curve->track < 0 ||
            size_t(p.curve->track) >= p.document.State().clip.tracks.size())
            return;
        p.document.Edit([&](auto& state) {
            state.clip.tracks.erase(state.clip.tracks.begin() + p.curve->track);
        });
        p.curve->track = 0;
        p.curve->selected.clear();
        p.Changed();
    });
    p.keys = new LuxUI::ListWidget(keyPage);
    p.keys->setObjectName(LUX_UI_TEXT("animationAssetKeys"));
    keyLayout->addWidget(p.keys, 1);
    p.keyTime = p.Spin(keyForm, "키 시간", 0, 86400);
    for (size_t i = 0; i < 4; ++i)
        p.values[i] = p.Spin(keyForm,
                             i == 0   ? "X / R"
                             : i == 1 ? "Y / G"
                             : i == 2 ? "Z / B"
                                      : "W / A",
                             -1e15, 1e15);
    p.interpolation = new LuxUI::ComboBox(keyPage);
    p.interpolation->addItems(
        {LUX_UI_TEXT("고정"), LUX_UI_TEXT("선형 / 회전 Slerp"), LUX_UI_TEXT("Cubic Hermite")});
    keyForm->addRow(LUX_UI_TEXT("보간"), p.interpolation);
    p.inTangent = p.Spin(keyForm, "들어오는 접선", -1e12, 1e12);
    p.outTangent = p.Spin(keyForm, "나가는 접선", -1e12, 1e12);
    p.Button(keyLayout, "현재 시간에 키 추가", [&p] { p.AddKey(); });
    p.Button(keyLayout, "선택 키 적용", [&p] {
        const int track = p.curve->track;
        if (p.document.State().isController || track < 0 ||
            size_t(track) >= p.document.State().clip.tracks.size() || p.curve->selected.empty())
            return;
        p.document.Edit([&](auto& state) {
            auto& source = state.clip.tracks[track];
            auto index = *p.curve->selected.begin();
            if (index >= source.keys.size())
                return;
            auto key = source.keys[index];
            key.time = std::clamp(p.keyTime->value(), 0., state.clip.duration);
            for (size_t i = 0; i < 4; ++i)
                key.value[i] = source.property == AnimationProperty::Rotation
                                   ? ToRadians(p.values[i]->value())
                                   : p.values[i]->value();
            key.inTangent[p.curve->axis] = p.inTangent->value();
            key.outTangent[p.curve->axis] = p.outTangent->value();
            key.interpolation = AnimationInterpolation(p.interpolation->currentIndex());
            if (key.time != source.keys[index].time &&
                std::any_of(source.keys.begin(), source.keys.end(),
                            [&](const auto& other) { return other.time == key.time; }))
                return;
            source.keys.erase(source.keys.begin() + index);
            source.SetKey(key);
        });
        p.curve->selected.clear();
        p.Changed();
    });
    right->addTab(keyPage, LUX_UI_TEXT("트랙 / 키"));
    auto* eventPage = new LuxUI::Widget(right);
    auto* eventLayout = new LuxUI::VBoxLayout(eventPage);
    auto* eventForm = new LuxUI::FormLayout;
    eventLayout->addLayout(eventForm);
    p.eventName = new LuxUI::LineEdit(eventPage);
    p.eventPayload = new LuxUI::LineEdit(eventPage);
    eventForm->addRow(LUX_UI_TEXT("이벤트 이름"), p.eventName);
    eventForm->addRow(LUX_UI_TEXT("전달 값"), p.eventPayload);
    auto* events = new LuxUI::ListWidget(eventPage);
    eventLayout->addWidget(events, 1);
    p.Button(eventLayout, "현재 시간에 이벤트 추가", [&p, events] {
        if (p.eventName->text().isEmpty())
            return;
        p.document.Edit([&](auto& state) {
            state.clip.events.push_back(
                {p.time->value(), Utf8(p.eventName->text()), Utf8(p.eventPayload->text())});
            std::stable_sort(state.clip.events.begin(), state.clip.events.end(),
                             [](const auto& a, const auto& b) { return a.time < b.time; });
        });
        events->clear();
        for (const auto& event : p.document.State().clip.events)
            events->addItem(LuxUI::String::number(event.time, 'f', 3) + LUX_UI_TEXT("  ") +
                            Text(event.name));
        p.Changed();
    });
    p.Button(eventLayout, "선택 이벤트 삭제", [&p, events] {
        int row = events->currentRow();
        if (row < 0 || size_t(row) >= p.document.State().clip.events.size())
            return;
        p.document.Edit(
            [&](auto& state) { state.clip.events.erase(state.clip.events.begin() + row); });
        delete events->takeItem(row);
        p.Changed();
    });
    right->addTab(eventPage, LUX_UI_TEXT("이벤트"));
    auto* controllerPage = new LuxUI::Widget(right);
    auto* controllerLayout = new LuxUI::VBoxLayout(controllerPage);
    auto* controllerForm = new LuxUI::FormLayout;
    controllerLayout->addLayout(controllerForm);
    p.layers = new LuxUI::ComboBox(controllerPage);
    controllerForm->addRow(LUX_UI_TEXT("레이어"), p.layers);
    auto* layerName = new LuxUI::LineEdit(controllerPage);
    controllerForm->addRow(LUX_UI_TEXT("이름"), layerName);
    auto* layerWeight = p.Spin(controllerForm, "가중치", 0, 1);
    layerWeight->setValue(1);
    auto* mask = new LuxUI::LineEdit(controllerPage);
    mask->setPlaceholderText(LUX_UI_TEXT("본/경로,본/경로"));
    controllerForm->addRow(LUX_UI_TEXT("마스크"), mask);
    auto* additive = new LuxUI::CheckBox(LUX_UI_TEXT("가산 레이어"), controllerPage);
    controllerLayout->addWidget(additive);
    p.Button(controllerLayout, "레이어 추가", [&p, layerName] {
        if (!p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            AnimatorLayer layer;
            layer.name = Utf8(layerName->text());
            if (layer.name.empty())
                layer.name = "Layer";
            state.controller.layers.push_back(layer);
            p.graph->layer = state.controller.layers.size() - 1;
        });
        p.Changed();
    });
    p.Button(controllerLayout, "레이어 설정 적용", [&p, layerName, layerWeight, mask, additive] {
        if (!p.document.State().isController ||
            p.graph->layer >= p.document.State().controller.layers.size())
            return;
        p.document.Edit([&](auto& state) {
            auto& layer = state.controller.layers[p.graph->layer];
            if (!layerName->text().isEmpty())
                layer.name = Utf8(layerName->text());
            layer.weight = layerWeight->value();
            layer.additive = additive->isChecked();
            layer.mask.clear();
            for (const auto& item : mask->text().split(','))
                if (!item.trimmed().isEmpty())
                    layer.mask.push_back(Utf8(item.trimmed()));
        });
        p.Changed();
    });
    auto* stateName = new LuxUI::LineEdit(controllerPage);
    controllerForm->addRow(LUX_UI_TEXT("상태 이름"), stateName);
    auto* stateClip = new LuxUI::LineEdit(controllerPage);
    controllerForm->addRow(LUX_UI_TEXT("클립 에셋"), stateClip);
    auto* stateSpeed = p.Spin(controllerForm, "상태 속도", -100, 100);
    stateSpeed->setValue(1);
    p.Button(controllerLayout, "클립 선택", [this, stateClip] {
        auto path = LuxUI::FileDialog::getOpenFileName(this, LUX_UI_TEXT("상태 클립"), {},
                                                       LUX_UI_TEXT("Animation (*.luxanim)"));
        if (!path.isEmpty())
            stateClip->setText(path);
    });
    p.Button(controllerLayout, "상태 추가", [&p, stateName, stateClip, stateSpeed] {
        if (!p.document.State().isController || stateName->text().isEmpty() ||
            stateClip->text().isEmpty())
            return;
        p.document.Edit([&](auto& state) {
            auto& layer = state.controller.layers[p.graph->layer];
            if (std::any_of(layer.states.begin(), layer.states.end(),
                            [&](const auto& s) { return s.name == Utf8(stateName->text()); }))
                return;
            AnimatorState item;
            item.name = Utf8(stateName->text());
            item.clip = Utf8(stateClip->text());
            item.speed = stateSpeed->value();
            layer.states.push_back(item);
            if (layer.entry.empty())
                layer.entry = item.name;
            p.graph->selected = layer.states.size() - 1;
        });
        p.Changed();
    });
    p.Button(controllerLayout, "선택 상태 설정 / 시작 상태", [&p, stateClip, stateSpeed] {
        if (!p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            auto& layer = state.controller.layers[p.graph->layer];
            if (p.graph->selected >= layer.states.size())
                return;
            auto& item = layer.states[p.graph->selected];
            if (!stateClip->text().isEmpty())
                item.clip = Utf8(stateClip->text());
            item.speed = stateSpeed->value();
            layer.entry = item.name;
        });
        p.Changed();
    });
    auto* parameter = new LuxUI::LineEdit(controllerPage);
    controllerForm->addRow(LUX_UI_TEXT("파라미터"), parameter);
    auto* parameterType = new LuxUI::ComboBox(controllerPage);
    parameterType->addItems({LUX_UI_TEXT("Float"), LUX_UI_TEXT("Integer"), LUX_UI_TEXT("Boolean"),
                             LUX_UI_TEXT("Trigger")});
    controllerForm->addRow(LUX_UI_TEXT("타입"), parameterType);
    auto* parameterValue = p.Spin(controllerForm, "값 / 임계값", -1e12, 1e12);
    p.Button(controllerLayout, "파라미터 추가 / 적용",
             [&p, parameter, parameterType, parameterValue] {
                 if (!p.document.State().isController || parameter->text().isEmpty())
                     return;
                 p.document.Edit([&](auto& state) {
                     auto& list = state.controller.parameters;
                     auto found = std::find_if(list.begin(), list.end(), [&](const auto& v) {
                         return v.name == Utf8(parameter->text());
                     });
                     AnimationParameter value{Utf8(parameter->text()),
                                              AnimationParameterType(parameterType->currentIndex()),
                                              parameterValue->value()};
                     if (found == list.end())
                         list.push_back(value);
                     else
                         *found = value;
                 });
                 p.Changed();
             });
    auto* transitionFrom = new LuxUI::LineEdit(controllerPage);
    auto* transitionTo = new LuxUI::LineEdit(controllerPage);
    controllerForm->addRow(LUX_UI_TEXT("전환 출발 / Any"), transitionFrom);
    controllerForm->addRow(LUX_UI_TEXT("전환 도착"), transitionTo);
    auto* fade = p.Spin(controllerForm, "전환 시간", 0, 60);
    fade->setValue(.2);
    auto* exit = p.Spin(controllerForm, "Exit Time / -1", -1, 100);
    exit->setValue(-1);
    auto* comparison = new LuxUI::ComboBox(controllerPage);
    comparison->addItems({LUX_UI_TEXT(">"), LUX_UI_TEXT("<"), LUX_UI_TEXT("=="), LUX_UI_TEXT("!="),
                          LUX_UI_TEXT("설정됨")});
    controllerForm->addRow(LUX_UI_TEXT("조건 비교"), comparison);
    p.Button(controllerLayout, "전환 추가 / 적용",
             [&p, transitionFrom, transitionTo, parameter, parameterValue, comparison, fade, exit] {
                 if (!p.document.State().isController || transitionTo->text().isEmpty())
                     return;
                 p.document.Edit([&](auto& state) {
                     auto& list = state.controller.layers[p.graph->layer].transitions;
                     AnimatorTransition value;
                     value.from = Utf8(transitionFrom->text());
                     value.to = Utf8(transitionTo->text());
                     value.duration = fade->value();
                     value.exitTime = exit->value();
                     if (!parameter->text().isEmpty())
                         value.conditions.push_back(
                             {Utf8(parameter->text()),
                              AnimationComparison(comparison->currentIndex()),
                              parameterValue->value()});
                     auto found = std::find_if(list.begin(), list.end(), [&](const auto& t) {
                         return t.from == value.from && t.to == value.to;
                     });
                     if (found == list.end())
                         list.push_back(value);
                     else
                         *found = value;
                 });
                 p.Changed();
             });
    p.Button(controllerLayout, "선택 상태에 Blend Tree 점 추가",
             [&p, parameter, parameterValue, stateClip] {
                 if (!p.document.State().isController || parameter->text().isEmpty() ||
                     stateClip->text().isEmpty())
                     return;
                 p.document.Edit([&](auto& state) {
                     auto& layer = state.controller.layers[p.graph->layer];
                     if (p.graph->selected >= layer.states.size())
                         return;
                     auto& item = layer.states[p.graph->selected];
                     item.blendParameter = Utf8(parameter->text());
                     auto& points = item.blendTree;
                     auto found =
                         std::find_if(points.begin(), points.end(), [&](const auto& point) {
                             return point.threshold == parameterValue->value();
                         });
                     AnimationBlendPoint point{parameterValue->value(), Utf8(stateClip->text())};
                     if (found == points.end())
                         points.push_back(point);
                     else
                         *found = point;
                     std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
                         return a.threshold < b.threshold;
                     });
                 });
                 p.Changed();
             });

    auto* parameterList = new LuxUI::ListWidget(controllerPage);
    parameterList->setObjectName(LUX_UI_TEXT("animationControllerParameters"));
    controllerLayout->addWidget(parameterList);
    auto* transitionList = new LuxUI::ListWidget(controllerPage);
    transitionList->setObjectName(LUX_UI_TEXT("animationControllerTransitions"));
    controllerLayout->addWidget(transitionList);
    connect(parameterList, &LuxUI::ListWidget::currentRowChanged, this,
            [&p, parameter, parameterType, parameterValue](int row) {
                if (p.refreshing || row < 0 ||
                    size_t(row) >= p.document.State().controller.parameters.size())
                    return;
                const auto& value = p.document.State().controller.parameters[size_t(row)];
                parameter->setText(Text(value.name));
                parameterType->setCurrentIndex(int(value.type));
                parameterValue->setValue(value.value);
            });
    connect(transitionList, &LuxUI::ListWidget::currentRowChanged, this,
            [&p, transitionFrom, transitionTo, fade, exit, parameter, parameterValue,
             comparison](int row) {
                if (p.refreshing || row < 0 ||
                    p.graph->layer >= p.document.State().controller.layers.size())
                    return;
                const auto& list = p.document.State().controller.layers[p.graph->layer].transitions;
                if (size_t(row) >= list.size())
                    return;
                const auto& value = list[size_t(row)];
                transitionFrom->setText(Text(value.from));
                transitionTo->setText(Text(value.to));
                fade->setValue(value.duration);
                exit->setValue(value.exitTime);
                if (!value.conditions.empty()) {
                    parameter->setText(Text(value.conditions[0].parameter));
                    parameterValue->setValue(value.conditions[0].value);
                    comparison->setCurrentIndex(int(value.conditions[0].comparison));
                }
            });
    p.Button(controllerLayout, "전환에 AND 조건 추가",
             [&p, transitionFrom, transitionTo, parameter, parameterValue, comparison] {
                 if (!p.document.State().isController || parameter->text().isEmpty())
                     return;
                 p.document.Edit([&](auto& state) {
                     auto& transitions = state.controller.layers[p.graph->layer].transitions;
                     auto found =
                         std::find_if(transitions.begin(), transitions.end(), [&](const auto& t) {
                             return t.from == Utf8(transitionFrom->text()) &&
                                    t.to == Utf8(transitionTo->text());
                         });
                     if (found != transitions.end())
                         found->conditions.push_back(
                             {Utf8(parameter->text()),
                              AnimationComparison(comparison->currentIndex()),
                              parameterValue->value()});
                 });
                 p.Changed();
             });
    p.Button(controllerLayout, "선택 전환 삭제", [&p, transitionList] {
        int row = transitionList->currentRow();
        if (row < 0 || !p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            auto& list = state.controller.layers[p.graph->layer].transitions;
            if (size_t(row) < list.size())
                list.erase(list.begin() + row);
        });
        p.Changed();
    });
    p.Button(controllerLayout, "선택 상태 삭제", [&p] {
        if (!p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            auto& layer = state.controller.layers[p.graph->layer];
            if (p.graph->selected >= layer.states.size())
                return;
            const auto name = layer.states[p.graph->selected].name;
            layer.states.erase(layer.states.begin() + p.graph->selected);
            std::erase_if(layer.transitions, [&](const auto& value) {
                return value.from == name || value.to == name;
            });
            if (layer.entry == name)
                layer.entry = layer.states.empty() ? std::string{} : layer.states[0].name;
            p.graph->selected = 0;
        });
        p.Changed();
    });
    p.Button(controllerLayout, "선택 상태를 시작 상태로 지정", [&p] {
        if (!p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            auto& layer = state.controller.layers[p.graph->layer];
            if (p.graph->selected < layer.states.size())
                layer.entry = layer.states[p.graph->selected].name;
        });
        p.Changed();
    });
    p.Button(controllerLayout, "Blend Tree 초기화", [&p] {
        if (!p.document.State().isController)
            return;
        p.document.Edit([&](auto& state) {
            auto& layer = state.controller.layers[p.graph->layer];
            if (p.graph->selected < layer.states.size()) {
                layer.states[p.graph->selected].blendTree.clear();
                layer.states[p.graph->selected].blendParameter.clear();
            }
        });
        p.Changed();
    });
    p.graph->changed = [&p, stateName, stateClip, stateSpeed, layerName, layerWeight, mask,
                        additive] {
        p.Refresh();
        if (!p.document.State().isController ||
            p.graph->layer >= p.document.State().controller.layers.size())
            return;
        const auto& layer = p.document.State().controller.layers[p.graph->layer];
        layerName->setText(Text(layer.name));
        layerWeight->setValue(layer.weight);
        additive->setChecked(layer.additive);
        std::string paths;
        for (const auto& value : layer.mask) {
            if (!paths.empty())
                paths += ",";
            paths += value;
        }
        mask->setText(Text(paths));
        if (p.graph->selected < layer.states.size()) {
            const auto& state = layer.states[p.graph->selected];
            stateName->setText(Text(state.name));
            stateClip->setText(Text(state.clip));
            stateSpeed->setValue(state.speed);
        }
    };
    auto* controllerHelp =
        new LuxUI::Label(LUX_UI_TEXT("그래프: 상태 드래그 이동 · Shift 드래그 전환 연결\n상태 / "
                                     "전환 이름과 파라미터를 입력해 설정합니다."),
                         controllerPage);
    controllerHelp->setWordWrap(true);
    controllerLayout->addWidget(controllerHelp);
    right->addTab(controllerPage, LUX_UI_TEXT("Animator"));
    auto* scroll = new LuxUI::ScrollArea(split);
    scroll->setWidgetResizable(true);
    right->setParent(scroll);
    scroll->setWidget(right);
    split->addWidget(scroll);
    split->setStretchFactor(1, 1);
    left->setMinimumWidth(170);
    scroll->setMinimumWidth(290);
    scroll->setMaximumWidth(420);
    center->setSizes({500, 220});
    p.status = new LuxUI::Label(LUX_UI_TEXT("애니메이션 에셋을 열거나 새 클립을 만드세요."), this);
    p.status->setWordWrap(true);
    layout->addWidget(p.status);
    connect(p.tracks, &LuxUI::ListWidget::currentRowChanged, this, [&p](int index) {
        if (p.refreshing)
            return;
        if (p.document.State().isController) {
            if (index >= 0)
                p.graph->layer = size_t(index);
        } else {
            p.curve->track = index;
            p.curve->selected.clear();
        }
        p.Refresh();
    });
    connect(p.keys, &LuxUI::ListWidget::currentRowChanged, this, [&p](int row) {
        if (p.refreshing || row < 0)
            return;
        p.curve->selected = {size_t(row)};
        p.Refresh();
    });
    connect(p.time, qOverload<double>(&LuxUI::DoubleSpinBox::valueChanged), this,
            [this, &p](double value) {
                if (!p.refreshing)
                    Seek(value);
            });
    auto clipChanged = [&p] {
        if (p.refreshing)
            return;
        p.document.Edit([&](auto& state) {
            state.clip.duration = p.duration->value();
            state.clip.framesPerSecond = p.fps->value();
            state.clip.wrap = AnimationWrap(p.wrap->currentIndex());
            state.clip.loop = state.clip.wrap != AnimationWrap::Once;
        });
        p.Changed();
    };
    connect(p.duration, &LuxUI::DoubleSpinBox::editingFinished, this, clipChanged);
    connect(p.fps, &LuxUI::DoubleSpinBox::editingFinished, this, clipChanged);
    connect(p.wrap, &LuxUI::ComboBox::currentIndexChanged, this, clipChanged);
    connect(columns, &LuxUI::DoubleSpinBox::editingFinished, this, [&p, columns, rows] {
        p.document.Edit([&](auto& state) {
            state.clip.spriteColumns = uint32(columns->value());
            state.clip.spriteRows = uint32(rows->value());
        });
        p.Changed();
    });
    connect(rows, &LuxUI::DoubleSpinBox::editingFinished, this, [&p, columns, rows] {
        p.document.Edit([&](auto& state) {
            state.clip.spriteColumns = uint32(columns->value());
            state.clip.spriteRows = uint32(rows->value());
        });
        p.Changed();
    });
    connect(modelClip, &LuxUI::ComboBox::currentIndexChanged, this, [&p, modelClip](int index) {
        if (p.refreshing)
            return;
        p.document.Edit([&](auto& state) { state.clip.modelClip = index - 1; });
        p.Changed();
    });
    connect(p.layers, &LuxUI::ComboBox::currentIndexChanged, this, [&p](int value) {
        if (!p.refreshing && value >= 0) {
            p.graph->layer = size_t(value);
            p.graph->selected = 0;
            p.graph->update();
        }
    });
    p.timer = new LuxUI::Timer(this);
    p.timer->setInterval(16);
    connect(p.timer, &LuxUI::Timer::timeout, this, [&p] {
        if (!p.playback.playing || !p.owner.isVisible())
            return;
        const double dt = std::min(.25, p.clock.restart() / 1000.);
        p.playback.speed = p.speed->value();
        AnimationFrame frame;
        try {
            if (p.document.State().isController) {
                if (!p.animator) {
                    p.ResetPreview();
                    p.playback.playing = true;
                }
                if (!p.animator)
                    return;
                p.animator->SetSpeed(p.playback.speed);
                frame = p.animator->Update(dt, true);
                p.playback.cursor = p.animator->GetTime();
            } else {
                const auto& clip = p.document.State().clip;
                p.fired = p.playback.Advance(dt, clip.duration, clip.GetWrap(), clip.events);
                frame.samples = clip.SampleTracks(p.playback.cursor);
                frame.events = p.fired;
                frame.spriteSheet = clip.spriteSheet;
                frame.spriteColumns = clip.spriteColumns;
                frame.spriteRows = clip.spriteRows;
                if (!clip.modelPath.empty())
                    frame.models.push_back({{},
                                            clip.modelPath,
                                            clip.modelClip,
                                            p.playback.Time(clip.duration, clip.GetWrap()),
                                            1});
            }
            if (p.target)
                p.preview.GetScene().ApplyAnimationFrame(p.target, frame, dt, false);
            p.refreshing = true;
            p.time->setValue(AnimationTime(p.playback.cursor, p.document.State().clip.duration,
                                           p.document.State().clip.GetWrap()));
            p.refreshing = false;
            p.curve->time = p.time->value();
            p.curve->update();
            p.view2D->RefreshScene();
            p.view3D->RefreshScene();
            if (!frame.events.empty())
                p.status->setText(LUX_UI_TEXT("이벤트: ") + Text(frame.events.back().name));
        } catch (const std::exception& error) {
            p.playback.playing = false;
            p.Error(error.what());
        }
    });
    p.timer->start();
    p.Refresh();
}

AnimationAssetEditor::~AnimationAssetEditor() {
    impl_->timer->stop();
}

AnimationAssetDocument& AnimationAssetEditor::Document() {
    return impl_->document;
}

bool AnimationAssetEditor::OpenAsset(const std::string& path) {
    std::string error;
    if (!impl_->document.Open(path, error)) {
        impl_->Error(error);
        return false;
    }
    impl_->curve->track = 0;
    impl_->curve->selected.clear();
    impl_->graph->layer = 0;
    impl_->ResetPreview();
    impl_->Refresh();
    Seek(0);
    return true;
}

bool AnimationAssetEditor::SaveAsset(const std::string& path) {
    std::string error;
    if (!impl_->document.Save(path, error)) {
        impl_->Error(error);
        return false;
    }
    impl_->status->setText(LUX_UI_TEXT("저장했습니다: ") + Text(path));
    return true;
}

void AnimationAssetEditor::Seek(double seconds) {
    auto& p = *impl_;
    if (!std::isfinite(seconds))
        return;
    const bool playing = p.playback.playing;
    if (!p.target || !p.preview.GetScene().IsAlive(p.target) ||
        (p.source.GetSelected() && p.source.GetSelected().GetId() != p.target))
        p.ResetPreview();
    p.playback.playing = playing;
    p.playback.cursor = p.document.State().isController
                            ? std::max(0., seconds)
                            : std::clamp(seconds, 0., p.document.State().clip.duration);
    AnimationFrame frame;
    try {
        if (p.document.State().isController) {
            if (!p.animator) {
                p.ResetPreview();
                p.playback.playing = playing;
            }
            if (p.animator) {
                p.animator->Seek(seconds);
                frame = p.animator->Update(0, false);
            }
        } else {
            const auto& clip = p.document.State().clip;
            frame.samples = clip.SampleTracks(p.playback.cursor, false);
            frame.spriteSheet = clip.spriteSheet;
            frame.spriteColumns = clip.spriteColumns;
            frame.spriteRows = clip.spriteRows;
            if (!clip.modelPath.empty())
                frame.models.push_back({{}, clip.modelPath, clip.modelClip, p.playback.cursor, 1});
        }
        if (p.target)
            p.preview.GetScene().ApplyAnimationFrame(p.target, frame, 0, false);
    } catch (const std::exception& error) {
        p.Error(error.what());
    }
    p.refreshing = true;
    p.time->setValue(p.playback.cursor);
    p.refreshing = false;
    p.curve->time = p.playback.cursor;
    p.curve->update();
    p.view2D->RefreshScene();
    p.view3D->RefreshScene();
}

void AnimationAssetEditor::Play(bool playing) {
    if (playing && !impl_->target)
        impl_->ResetPreview();
    impl_->playback.playing = playing;
    impl_->clock.restart();
    if (impl_->animator)
        impl_->animator->SetPlaying(playing);
}

double AnimationAssetEditor::Time() const {
    return impl_->playback.cursor;
}

bool AnimationAssetEditor::PreviewSourceUnchanged() const {
    std::string value;
    return impl_->source.GetScene().SerializeToString(value) && value == impl_->sourceSnapshot;
}

bool AnimationAssetEditor::ConfirmDiscard() {
    if (!impl_->document.Dirty())
        return true;
    auto answer = LuxUI::MessageBox::question(
        this, LUX_UI_TEXT("애니메이션 저장"),
        LUX_UI_TEXT("변경한 애니메이션 에셋을 저장하시겠습니까?"),
        LuxUI::MessageBox::Save | LuxUI::MessageBox::Discard | LuxUI::MessageBox::Cancel,
        LuxUI::MessageBox::Save);
    if (answer == LuxUI::MessageBox::Cancel)
        return false;
    if (answer == LuxUI::MessageBox::Discard)
        return true;
    impl_->Save();
    return !impl_->document.Dirty();
}

bool AnimationAssetEditor::event(LuxUI::Event* event) {
    if (event->type() == LuxUI::Event::Hide)
        Play(false);
    if (event->type() == LuxUI::Event::ShortcutOverride) {
        auto* key = static_cast<LuxUI::KeyEvent*>(event);
        if (key->matches(LuxUI::KeySequence::Undo) || key->matches(LuxUI::KeySequence::Redo) ||
            key->matches(LuxUI::KeySequence::Save) || key->matches(LuxUI::KeySequence::Copy) ||
            key->matches(LuxUI::KeySequence::Cut) || key->matches(LuxUI::KeySequence::Paste) ||
            key->matches(LuxUI::KeySequence::SelectAll) || key->key() == LuxUI::Flags::Key_Delete ||
            key->key() == LuxUI::Flags::Key_Space) {
            event->accept();
            return true;
        }
    }
    return LuxUI::Widget::event(event);
}

void AnimationAssetEditor::keyPressEvent(LuxUI::KeyEvent* event) {
    if (event->matches(LuxUI::KeySequence::Undo)) {
        if (impl_->document.Undo())
            impl_->Changed();
        event->accept();
    } else if (event->matches(LuxUI::KeySequence::Redo)) {
        if (impl_->document.Redo())
            impl_->Changed();
        event->accept();
    } else if (event->matches(LuxUI::KeySequence::Save)) {
        impl_->Save();
        event->accept();
    } else if (event->key() == LuxUI::Flags::Key_Space) {
        Play(!impl_->playback.playing);
        event->accept();
    } else
        event->ignore();
}
} // namespace LuxEditor
