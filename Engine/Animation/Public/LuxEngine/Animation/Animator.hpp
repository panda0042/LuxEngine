#pragma once
#include <LuxEngine/Animation/AnimationEngine.hpp>
#include <functional>
#include <memory>

namespace LuxEngine {
enum class AnimationParameterType { Float, Integer, Boolean, Trigger };
enum class AnimationComparison { Greater, Less, Equal, NotEqual, Set };

struct AnimationParameter {
    std::string name{};
    AnimationParameterType type = AnimationParameterType::Float;
    double value = 0;
};

struct AnimationCondition {
    std::string parameter{};
    AnimationComparison comparison = AnimationComparison::Greater;
    double value = 0;
};

struct AnimationBlendPoint {
    double threshold = 0;
    std::string clip{};
};

struct AnimatorState {
    std::string name{};
    std::string clip{};
    double speed = 1;
    std::string blendParameter{};
    std::vector<AnimationBlendPoint> blendTree{};
    Vec2 editorPosition{};
};

struct AnimatorTransition {
    std::string from, to; // Empty 'from' is Any State.
    double duration = .2;
    double exitTime = -1; // Normalized time; negative disables the exit-time condition.
    std::vector<AnimationCondition> conditions{};
};

struct AnimatorLayer {
    std::string name = "Base";
    std::string entry{};
    double weight = 1;
    bool additive = false;
    std::vector<std::string> mask{};
    std::vector<AnimatorState> states{};
    std::vector<AnimatorTransition> transitions{};
};

class LUX_API AnimatorController {
public:
    std::vector<AnimationParameter> parameters{};
    std::vector<AnimatorLayer> layers{};
    std::string sourcePath{};
    bool Validate(std::string& error) const;
    bool SerializeForFile(const std::string& path, std::string& output, std::string& error) const;
    bool Save(const std::string& path, std::string& error) const;
    static Ref<AnimatorController> Load(const std::string& path, std::string& error);
};

struct AnimationModelSample {
    std::string target, path;
    int32 clip = -1;
    double time = 0, weight = 1;
    size_t layer = 0;
    bool additive = false;
    std::vector<std::string> mask{};
    double previousTime = 0;
    double cycles = 0;
    std::string rootMotionTarget{};
};

struct AnimationFrame {
    std::optional<Vec3> rootDelta;
    std::vector<AnimationSample> samples{};
    std::vector<AnimationEvent> events{};
    std::vector<AnimationModelSample> models{};
    std::string spriteSheet{};
    uint32 spriteColumns = 1, spriteRows = 1;

    struct RootMotionSample {
        std::string target;
        Vec3 delta;
        bool modelBinding = false;
    };

    std::vector<RootMotionSample> childRootMotion{};
};

class LUX_API AnimatorPlayer {
public:
    AnimatorPlayer();
    ~AnimatorPlayer();
    AnimatorPlayer(const AnimatorPlayer&);
    AnimatorPlayer& operator=(const AnimatorPlayer&);
    bool SetController(Ref<AnimatorController> controller, std::string& error);
    bool SetParameter(const std::string& name, double value);
    bool Trigger(const std::string& name);
    double GetParameter(const std::string& name) const;
    bool Play(const std::string& state, double fade = 0, size_t layer = 0);
    void Seek(double seconds, size_t layer = 0);
    AnimationFrame Update(double dt, bool emitEvents = true);
    std::string GetState(size_t layer = 0) const;
    double GetTime(size_t layer = 0) const;
    void SetPlaying(bool playing);
    bool SetSpeed(double speed);
    double GetSpeed() const;
    double GetDuration(size_t layer = 0) const;
    bool IsPlaying() const;

private:
    friend class Scene;
    using PropertyReader =
        std::function<std::optional<std::array<double, 4>>(const AnimationSample&)>;
    AnimationFrame Update(double dt, bool emitEvents, const PropertyReader& readProperty,
                          bool extractRootMotion);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct AnimatorComponent {
    LUX_API AnimatorComponent();
    LUX_API AnimatorComponent(const AnimatorComponent& other);
    LUX_API AnimatorComponent& operator=(const AnimatorComponent& other);
    LUX_API AnimatorComponent(AnimatorComponent&&) noexcept;
    LUX_API AnimatorComponent& operator=(AnimatorComponent&&) noexcept;
    LUX_API ~AnimatorComponent();
    std::string controllerPath{};
    bool playing = true;
    bool rootMotion = false;
    std::vector<AnimationParameter> parameters{};
    Ref<AnimatorPlayer> player;
    std::string loadedPath{};
    std::vector<AnimationEvent> pendingEvents{};
};
} // namespace LuxEngine
