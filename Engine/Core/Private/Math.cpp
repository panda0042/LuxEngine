#include <LuxEngine/Core/Math.hpp>
#include <algorithm>
#include <array>
#include <limits>

namespace LuxEngine {
Vec2 Vec2::operator+(const Vec2& o) const {
    return {x + o.x, y + o.y};
}

Vec2 Vec2::operator-(const Vec2& o) const {
    return {x - o.x, y - o.y};
}

Vec2 Vec2::operator*(double scalar) const {
    return {x * scalar, y * scalar};
}

Vec2& Vec2::operator+=(const Vec2& o) {
    x += o.x;
    y += o.y;
    return *this;
}

Vec3 Vec3::operator+(const Vec3& o) const {
    return {x + o.x, y + o.y, z + o.z};
}

Vec3 Vec3::operator-(const Vec3& o) const {
    return {x - o.x, y - o.y, z - o.z};
}

Vec3 Vec3::operator*(double scalar) const {
    return {x * scalar, y * scalar, z * scalar};
}

Vec3& Vec3::operator+=(const Vec3& o) {
    x += o.x;
    y += o.y;
    z += o.z;
    return *this;
}

double Vec3::Dot(const Vec3& o) const {
    return x * o.x + y * o.y + z * o.z;
}

Vec3 Vec3::Cross(const Vec3& o) const {
    return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
}

Vec3 Vec3::Normalized() const {
    const double len = std::hypot(x, y, z);
    if (len > 0.00001f) {
        return {x / len, y / len, z / len};
    }
    return *this;
}

Mat4 Mat4::Identity() {
    return Mat4{};
}

Mat4 Mat4::Orthographic(double left, double right, double bottom, double top, double nearClip,
                        double farClip) {
    if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(bottom) ||
        !std::isfinite(top) || !std::isfinite(nearClip) || !std::isfinite(farClip) ||
        left == right || bottom == top || nearClip == farClip)
        throw std::invalid_argument("Invalid orthographic projection");
    Mat4 result{};
    result.elements[0] = 2.0f / (right - left);
    result.elements[5] = 2.0f / (top - bottom);
    result.elements[10] = -1.0f / (farClip - nearClip);
    result.elements[12] = -(right + left) / (right - left);
    result.elements[13] = -(top + bottom) / (top - bottom);
    result.elements[14] = -nearClip / (farClip - nearClip);
    result.elements[15] = 1.0f;
    return result;
}

Mat4 Mat4::Perspective(double fovRadians, double aspect, double nearClip, double farClip) {
    Mat4 result{};
    if (!std::isfinite(fovRadians) || !std::isfinite(aspect) || !std::isfinite(nearClip) ||
        !std::isfinite(farClip) || fovRadians <= 0 || fovRadians >= std::numbers::pi_v<double> ||
        aspect <= 0 || nearClip <= 0 || farClip <= nearClip)
        throw std::invalid_argument("Invalid perspective projection");
    const auto representable = [](double value) {
        // Projection coefficients cross the GPU float boundary; world/view translations do not.
        if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
            throw std::invalid_argument("Perspective projection exceeds GPU range");
        const float packed = static_cast<float>(value);
        if (!std::isfinite(packed) || (value != 0 && packed == 0))
            throw std::invalid_argument("Perspective projection exceeds GPU range");
        return value;
    };
    const double near = nearClip, far = farClip;
    const double tanHalfFov = std::tan(static_cast<double>(fovRadians) * .5);
    result.elements[0] = representable(1.0 / (static_cast<double>(aspect) * tanHalfFov));
    result.elements[5] = representable(1.0 / tanHalfFov);
    result.elements[10] = representable(far / (near - far));
    result.elements[11] = -1.0f;
    result.elements[14] = representable(-(far * near) / (far - near));
    result.elements[15] = 0.0f;
    return result;
}

Mat4 Mat4::Translate(const Vec3& translation) {
    Mat4 result = Identity();
    result.elements[12] = translation.x;
    result.elements[13] = translation.y;
    result.elements[14] = translation.z;
    return result;
}

Mat4 Mat4::Scale(const Vec3& scale) {
    Mat4 result = Identity();
    result.elements[0] = scale.x;
    result.elements[5] = scale.y;
    result.elements[10] = scale.z;
    return result;
}

Mat4 Mat4::RotateX(double angle) {
    Mat4 result;
    const double c = std::cos(angle), s = std::sin(angle);
    result.elements[5] = c;
    result.elements[6] = s;
    result.elements[9] = -s;
    result.elements[10] = c;
    return result;
}

Mat4 Mat4::RotateY(double angle) {
    Mat4 result;
    const double c = std::cos(angle), s = std::sin(angle);
    result.elements[0] = c;
    result.elements[2] = -s;
    result.elements[8] = s;
    result.elements[10] = c;
    return result;
}

Mat4 Mat4::RotateZ(double angleRadians) {
    Mat4 result = Identity();
    const double c = std::cos(angleRadians);
    const double s = std::sin(angleRadians);
    result.elements[0] = c;
    result.elements[1] = s;
    result.elements[4] = -s;
    result.elements[5] = c;
    return result;
}

Mat4 Mat4::LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 forward = (target - eye).Normalized();
    if (forward.x == 0.0f && forward.y == 0.0f && forward.z == 0.0f) {
        forward = {0.0f, 0.0f, -1.0f};
    }
    Vec3 right = forward.Cross(up).Normalized();
    if (right.x == 0.0f && right.y == 0.0f && right.z == 0.0f) {
        const Vec3 alt =
            std::abs(forward.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
        right = forward.Cross(alt).Normalized();
    }
    const Vec3 trueUp = right.Cross(forward);

    Mat4 result = Identity();
    result.elements[0] = right.x;
    result.elements[4] = right.y;
    result.elements[8] = right.z;

    result.elements[1] = trueUp.x;
    result.elements[5] = trueUp.y;
    result.elements[9] = trueUp.z;

    result.elements[2] = -forward.x;
    result.elements[6] = -forward.y;
    result.elements[10] = -forward.z;

    result.elements[12] = -right.Dot(eye);
    result.elements[13] = -trueUp.Dot(eye);
    result.elements[14] = forward.Dot(eye);
    return result;
}

Mat4 Mat4::operator*(const Mat4& o) const {
    Mat4 res{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += elements[k * 4 + r] * o.elements[c * 4 + k];
            }
            res.elements[c * 4 + r] = sum;
        }
    }
    return res;
}

double ToRadians(double degrees) {
    return degrees * (static_cast<double>(std::numbers::pi) / 180.0f);
}

Vec3 Mat4::TransformPoint(Vec3 v) const {
    return {elements[0] * v.x + elements[4] * v.y + elements[8] * v.z + elements[12],
            elements[1] * v.x + elements[5] * v.y + elements[9] * v.z + elements[13],
            elements[2] * v.x + elements[6] * v.y + elements[10] * v.z + elements[14]};
}

Vec3 Mat4::TransformDirection(Vec3 v) const {
    return {elements[0] * v.x + elements[4] * v.y + elements[8] * v.z,
            elements[1] * v.x + elements[5] * v.y + elements[9] * v.z,
            elements[2] * v.x + elements[6] * v.y + elements[10] * v.z};
}

Mat4 Mat4::InverseAffine() const {
    const Vec3 x{elements[0], elements[1], elements[2]}, y{elements[4], elements[5], elements[6]},
        z{elements[8], elements[9], elements[10]};
    const double determinant = x.Dot(y.Cross(z));
    if (!std::isfinite(determinant) || std::abs(determinant) < 1e-24)
        throw std::invalid_argument("Singular affine transform");
    const Vec3 a = y.Cross(z) * (1 / determinant), b = z.Cross(x) * (1 / determinant),
               c = x.Cross(y) * (1 / determinant);
    Mat4 result;
    result.elements[0] = a.x;
    result.elements[4] = a.y;
    result.elements[8] = a.z;
    result.elements[1] = b.x;
    result.elements[5] = b.y;
    result.elements[9] = b.z;
    result.elements[2] = c.x;
    result.elements[6] = c.y;
    result.elements[10] = c.z;
    const auto offset = result.TransformDirection({-elements[12], -elements[13], -elements[14]});
    result.elements[12] = offset.x;
    result.elements[13] = offset.y;
    result.elements[14] = offset.z;
    return result;
}

namespace {
using Value = std::array<double, 4>;

Value Quaternion(Value e) {
    const double x = e[0] * .5, y = e[1] * .5, z = e[2] * .5;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y),
                 cz = std::cos(z), sz = std::sin(z);
    return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz};
}

Value Euler(Value q) {
    return {std::atan2(2 * (q[3] * q[0] + q[1] * q[2]), 1 - 2 * (q[0] * q[0] + q[1] * q[1])),
            std::asin(std::clamp(2 * (q[3] * q[1] - q[2] * q[0]), -1.0, 1.0)),
            std::atan2(2 * (q[3] * q[2] + q[0] * q[1]), 1 - 2 * (q[1] * q[1] + q[2] * q[2])), 0};
}
} // namespace

Vec3 SlerpRotation(Vec3 av, Vec3 bv, double weight) {
    const Value a{av.x, av.y, av.z, 0}, b{bv.x, bv.y, bv.z, 0};
    auto qa = Quaternion(a), qb = Quaternion(b);
    double dot = 0;
    for (size_t i = 0; i < 4; ++i)
        dot += qa[i] * qb[i];
    if (dot < 0) {
        for (auto& v : qb)
            v = -v;
        dot = -dot;
    }
    double x = 1 - weight, y = weight;
    if (dot < .9995) {
        const double angle = std::acos(std::clamp(dot, -1.0, 1.0)), inverse = 1 / std::sin(angle);
        x = std::sin((1 - weight) * angle) * inverse;
        y = std::sin(weight * angle) * inverse;
    }
    Value result;
    double length = 0;
    for (size_t i = 0; i < 4; ++i) {
        result[i] = qa[i] * x + qb[i] * y;
        length += result[i] * result[i];
    }
    for (auto& v : result)
        v /= std::sqrt(length);
    auto e = Euler(result);
    return {e[0], e[1], e[2]};
}

Vec3 AdditiveRotation(Vec3 base, Vec3 reference, Vec3 value, double weight) {
    auto multiply = [](Value a, Value b) -> Value {
        return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    auto r = Quaternion({reference.x, reference.y, reference.z, 0});
    for (size_t i = 0; i < 3; ++i)
        r[i] = -r[i];
    const auto delta = Euler(multiply(r, Quaternion({value.x, value.y, value.z, 0})));
    const auto scaled = SlerpRotation({}, {delta[0], delta[1], delta[2]}, weight);
    const auto composed = Euler(multiply(Quaternion({base.x, base.y, base.z, 0}),
                                         Quaternion({scaled.x, scaled.y, scaled.z, 0})));
    return {composed[0], composed[1], composed[2]};
}
} // namespace LuxEngine
