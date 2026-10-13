#pragma once
#include <cmath>

// Minimal vector / quaternion math. Same convention as Teardown and OpenXR:
// right-handed, +Y up, -Z forward, quaternions stored as (x, y, z, w).

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Pose { Quat q; Vec3 p; };

inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }

inline Quat operator*(Quat a, Quat b)
{
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline Quat Conjugate(Quat q) { return { -q.x, -q.y, -q.z, q.w }; }
inline Quat Normalize(Quat q)
{
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (l < 1e-8f) return {};
    return { q.x / l, q.y / l, q.z / l, q.w / l };
}
inline Vec3 Rotate(Quat q, Vec3 v)
{
    Vec3 u{ q.x, q.y, q.z };
    Vec3 t = Cross(u, v) * 2.0f;
    return v + t * q.w + Cross(u, t);
}
inline Quat AxisAngle(Vec3 axis, float rad)
{
    float s = std::sin(rad * 0.5f);
    return { axis.x * s, axis.y * s, axis.z * s, std::cos(rad * 0.5f) };
}
// Yaw (rotation about +Y) of a rotation, in radians. 0 = looking down -Z.
inline float YawOf(Quat q)
{
    Vec3 f = Rotate(q, { 0, 0, -1 });
    return std::atan2(-f.x, -f.z);
}
inline Pose Mul(const Pose& a, const Pose& b) // a * b  (b expressed in a's frame)
{
    return { a.q * b.q, a.p + Rotate(a.q, b.p) };
}
inline Pose Inverse(const Pose& a)
{
    Quat iq = Conjugate(a.q);
    return { iq, Rotate(iq, a.p * -1.0f) };
}

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
