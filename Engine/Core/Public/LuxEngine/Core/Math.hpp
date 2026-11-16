#pragma once

#include <LuxEngine/Core/Core.hpp>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace LuxEngine {

struct Vec2 {
    double x = 0.0f;
    double y = 0.0f;

    constexpr Vec2();

    constexpr Vec2(double inX, double inY);

    LUX_API Vec2 operator+(const Vec2& o) const;

    LUX_API Vec2 operator-(const Vec2& o) const;

    LUX_API Vec2 operator*(double scalar) const;

    LUX_API Vec2& operator+=(const Vec2& o);
};

struct Vec3 {
    double x = 0.0f;
    double y = 0.0f;
    double z = 0.0f;

    constexpr Vec3();

    constexpr Vec3(double inX, double inY, double inZ);

    LUX_API Vec3 operator+(const Vec3& o) const;

    LUX_API Vec3 operator-(const Vec3& o) const;

    LUX_API Vec3 operator*(double scalar) const;

    LUX_API Vec3& operator+=(const Vec3& o);

    LUX_API double Dot(const Vec3& o) const;

    LUX_API Vec3 Cross(const Vec3& o) const;

    LUX_API Vec3 Normalized() const;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    constexpr Vec4();

    constexpr Vec4(float inX, float inY, float inZ, float inW);
};

struct Mat4 {
    double elements[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                           0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};

    LUX_API static Mat4 Identity();

    LUX_API static Mat4 Orthographic(double left, double right, double bottom, double top,
                                     double nearClip, double farClip);

    LUX_API static Mat4 Perspective(double fovRadians, double aspect, double nearClip,
                                    double farClip);

    LUX_API static Mat4 Translate(const Vec3& translation);

    LUX_API static Mat4 Scale(const Vec3& scale);

    LUX_API static Mat4 RotateX(double angle);

    LUX_API static Mat4 RotateY(double angle);

    LUX_API static Mat4 RotateZ(double angleRadians);

    LUX_API static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up);

    LUX_API Mat4 operator*(const Mat4& o) const;
    LUX_API Mat4 InverseAffine() const;
    LUX_API Vec3 TransformPoint(Vec3 point) const;
    LUX_API Vec3 TransformDirection(Vec3 direction) const;
};

LUX_API Vec3 AdditiveRotation(Vec3 base, Vec3 reference, Vec3 value, double weight);

LUX_API Vec3 SlerpRotation(Vec3 a, Vec3 b, double weight);

LUX_API double ToRadians(double degrees);

} // namespace LuxEngine

#include <LuxEngine/Core/Math.inl>
