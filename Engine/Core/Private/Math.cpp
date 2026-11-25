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
    const auto finite = [](Vec3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    if (!finite(eye) || !finite(target) || !finite(up))
        throw std::invalid_argument("Invalid view coordinates");

    const auto largestComponent = [](Vec3 v) {
        return std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    };
    const auto scaled = [](Vec3 v, double scale) {
        return Vec3{v.x / scale, v.y / scale, v.z / scale};
    };
    const auto unit = [&](Vec3 v) {
        const double scale = largestComponent(v);
        if (scale == 0)
            return Vec3{};
        const auto direction = scaled(v, scale);
        return direction * (1 / std::sqrt(direction.Dot(direction)));
    };
    auto direction = target - eye;
    if (!finite(direction)) {
        const double scale = std::max(largestComponent(eye), largestComponent(target));
        direction = scaled(target, scale) - scaled(eye, scale);
    }
    auto forward = unit(direction);
    if (forward.Dot(forward) == 0)
        forward = {0, 0, -1};

    auto cross = forward.Cross(unit(up));
    if (cross.Dot(cross) < 1e-24) {
        const Vec3 alternate = std::abs(forward.y) > .99 ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        cross = forward.Cross(alternate);
    }
    const Vec3 right = unit(cross);
    const Vec3 trueUp = right.Cross(forward);
    const auto projectEye = [&](Vec3 axis) {
        double value = axis.Dot(eye);
        if (!std::isfinite(value)) {
            const double scale = largestComponent(eye);
            value = axis.Dot(scaled(eye, scale)) * scale;
        }
        if (!std::isfinite(value))
            throw std::invalid_argument("View translation exceeds the representable range");
        return value;
    };

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

    result.elements[12] = -projectEye(right);
    result.elements[13] = -projectEye(trueUp);
    result.elements[14] = projectEye(forward);
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
using Quaternion = std::array<double, 4>;

Quaternion ToQuaternion(Vec3 euler) {
    const double x = euler.x * .5, y = euler.y * .5, z = euler.z * .5;
    const double cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y),
                 cz = std::cos(z), sz = std::sin(z);
    return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz};
}

Vec3 ToEuler(Quaternion q) {
    const auto [x, y, z, w] = q;
    const double xx = x * x, yy = y * y, zz = z * z, ww = w * w;
    const double r00 = (ww - yy) + (xx - zz), r10 = 2 * (w * z + x * y);
    const double cy = std::hypot(r00, r10);
    const double pitch = std::atan2(2 * (w * y - z * x), cy);
    if (cy < 1e-8) {
        const double r11 = (ww - xx) + (yy - zz), r12 = 2 * (y * z - w * x);
        return {std::atan2(-r12, r11), pitch, 0};
    }
    return {std::atan2(2 * (w * x + y * z), (ww - yy) + (zz - xx)), pitch, std::atan2(r10, r00)};
}

Quaternion Multiply(Quaternion a, Quaternion b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

Quaternion Slerp(Quaternion a, Quaternion b, double weight) {
    double dot = 0;
    for (size_t i = 0; i < a.size(); ++i)
        dot += a[i] * b[i];
    if (dot < 0) {
        for (auto& v : b)
            v = -v;
        dot = -dot;
    }
    double x = 1 - weight, y = weight;
    if (dot < .9995) {
        const double angle = std::acos(std::clamp(dot, -1.0, 1.0)), inverse = 1 / std::sin(angle);
        x = std::sin((1 - weight) * angle) * inverse;
        y = std::sin(weight * angle) * inverse;
    }
    Quaternion result;
    double length = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = a[i] * x + b[i] * y;
        length += result[i] * result[i];
    }
    for (auto& v : result)
        v /= std::sqrt(length);
    return result;
}
} // namespace

Vec3 SlerpRotation(Vec3 a, Vec3 b, double weight) {
    return ToEuler(Slerp(ToQuaternion(a), ToQuaternion(b), weight));
}

Vec3 AdditiveRotation(Vec3 base, Vec3 reference, Vec3 value, double weight) {
    const auto [x, y, z, w] = ToQuaternion(reference);
    const auto delta = Multiply({-x, -y, -z, w}, ToQuaternion(value));
    const auto scaled = Slerp({0, 0, 0, 1}, delta, weight);
    return ToEuler(Multiply(ToQuaternion(base), scaled));
}
} // namespace LuxEngine
