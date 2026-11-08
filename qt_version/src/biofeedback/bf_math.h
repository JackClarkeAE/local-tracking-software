#pragma once
// Minimal 3D vector + quaternion helpers for the anatomical biofeedback engine.
// Header-only, no Qt dependency, so the engine can be unit-tested on its own.

#include <cmath>

namespace bf {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }
inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }

// Returns false (and leaves `out` untouched) when v is too short to normalise.
inline bool tryNormalize(const Vec3& v, Vec3& out, float eps = 1e-6f) {
    const float len = length(v);
    if (len < eps) return false;
    out = v / len;
    return true;
}

inline Vec3 normalizeOr(const Vec3& v, const Vec3& fallback) {
    Vec3 n;
    return tryNormalize(v, n) ? n : fallback;
}

// Component of v perpendicular to unit vector n
inline Vec3 rejectFrom(const Vec3& v, const Vec3& n) { return v - n * dot(v, n); }

// Unit quaternion (w + xi + yj + zk). Used for all rotations of the distal
// chain so a joint is always rotated about an explicit anatomical axis.
struct Quat {
    float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;

    static Quat identity() { return {}; }

    // Rotation of `angle` radians about unit `axis`. Algebraically,
    // fromAxisAngle(u × w, +a) turns u toward w for any orthonormal u, w,
    // independent of whether the camera's coordinates are left- or
    // right-handed (Kinect and ZED differ).
    static Quat fromAxisAngle(const Vec3& axis, float angle) {
        const float h = angle * 0.5f;
        const float s = std::sin(h);
        Quat q;
        q.w = std::cos(h);
        q.x = axis.x * s;
        q.y = axis.y * s;
        q.z = axis.z * s;
        return q;
    }

    // Shortest-arc rotation taking unit vector a onto unit vector b.
    static Quat fromTwoVectors(const Vec3& a, const Vec3& b) {
        const float d = dot(a, b);
        if (d > 0.999999f) return identity();
        if (d < -0.999999f) {
            // 180°: any axis perpendicular to a
            Vec3 axis = cross(Vec3(1, 0, 0), a);
            if (length(axis) < 1e-4f) axis = cross(Vec3(0, 1, 0), a);
            return fromAxisAngle(normalizeOr(axis, Vec3(0, 0, 1)), 3.14159265358979f);
        }
        const Vec3 c = cross(a, b);
        Quat q;
        q.w = 1.0f + d;
        q.x = c.x;
        q.y = c.y;
        q.z = c.z;
        return q.normalized();
    }

    Quat normalized() const {
        const float n = std::sqrt(w * w + x * x + y * y + z * z);
        if (n < 1e-12f) return identity();
        return {w / n, x / n, y / n, z / n};
    }

    Quat operator*(const Quat& o) const {
        return {w * o.w - x * o.x - y * o.y - z * o.z,
                w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w};
    }

    Vec3 rotate(const Vec3& v) const {
        // v' = v + 2w(q × v) + 2 q × (q × v)
        const Vec3 q(x, y, z);
        const Vec3 t = cross(q, v) * 2.0f;
        return v + t * w + cross(q, t);
    }
};

} // namespace bf
