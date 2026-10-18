#pragma once
// =====================================================================
//  types.h
//  Tipos matematicos basicos. Sem dependencia de Windows, ImGui ou MinHook.
//  Pode ser incluido sozinho por qualquer modulo.
// =====================================================================

#include <cmath>
#include <cstdint>
#include <algorithm>

// ---------------------------------------------------------------------
//  Vec2
// ---------------------------------------------------------------------
struct Vec2 {
    float x, y;

    Vec2() : x(0.f), y(0.f) {}
    Vec2(float a, float b) : x(a), y(b) {}

    Vec2 operator+(const Vec2& o) const { return Vec2(x + o.x, y + o.y); }
    Vec2 operator-(const Vec2& o) const { return Vec2(x - o.x, y - o.y); }
    Vec2 operator*(float s)       const { return Vec2(x * s, y * s); }
    Vec2 operator/(float s)       const { return Vec2(x / s, y / s); }
    Vec2 operator-()              const { return Vec2(-x, -y); }

    Vec2& operator+=(const Vec2& o) { x += o.x; y += o.y; return *this; }
    Vec2& operator-=(const Vec2& o) { x -= o.x; y -= o.y; return *this; }
    Vec2& operator*=(float s) { x *= s;   y *= s;   return *this; }
    Vec2& operator/=(float s) { x /= s;   y /= s;   return *this; }

    bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Vec2& o) const { return !(*this == o); }

    float dot(const Vec2& o) const { return x * o.x + y * o.y; }
    float cross(const Vec2& o) const { return x * o.y - y * o.x; }

    float len2() const { return x * x + y * y; }
    float len()  const { return std::sqrt(len2()); }
    float dist(const Vec2& o) const { return (*this - o).len(); }

    Vec2  normalized() const {
        const float l = len();
        return (l > 1.0e-8f) ? (*this / l) : Vec2(0.f, 0.f);
    }
    Vec2& normalize() {
        const float l = len();
        if (l > 1.0e-8f) { x /= l; y /= l; }
        return *this;
    }

    bool zero() const { return x == 0.f && y == 0.f; }

    bool isFinite() const { return std::isfinite(x) && std::isfinite(y); }
    bool isSane(float limit = 1.0e7f) const {
        return isFinite() && std::fabs(x) <= limit && std::fabs(y) <= limit;
    }
};

inline Vec2 operator*(float s, const Vec2& v) { return v * s; }

// ---------------------------------------------------------------------
//  Vec3
// ---------------------------------------------------------------------
struct Vec3 {
    float x, y, z;

    Vec3() : x(0.f), y(0.f), z(0.f) {}
    Vec3(float a, float b, float c) : x(a), y(b), z(c) {}

    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(float s)       const { return Vec3(x * s, y * s, z * s); }
    Vec3 operator/(float s)       const { return Vec3(x / s, y / s, z / s); }
    Vec3 operator-()              const { return Vec3(-x, -y, -z); }

    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s;   y *= s;   z *= s;   return *this; }
    Vec3& operator/=(float s) { x /= s;   y /= s;   z /= s;   return *this; }

    bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    bool operator!=(const Vec3& o) const { return !(*this == o); }

    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3  cross(const Vec3& o) const {
        return Vec3(y * o.z - z * o.y,
            z * o.x - x * o.z,
            x * o.y - y * o.x);
    }

    float len2() const { return x * x + y * y + z * z; }
    float len()  const { return std::sqrt(len2()); }
    float dist(const Vec3& o) const { return (*this - o).len(); }

    Vec3  normalized() const {
        const float l = len();
        return (l > 1.0e-8f) ? (*this / l) : Vec3(0.f, 0.f, 0.f);
    }
    Vec3& normalize() {
        const float l = len();
        if (l > 1.0e-8f) { x /= l; y /= l; z /= l; }
        return *this;
    }

    bool zero() const { return x == 0.f && y == 0.f && z == 0.f; }

    // (0,0,0) e um ponto VALIDO: zero() nao serve para detectar falha de
    // resolucao de posicao. Use isFinite() para NaN/Inf e isSane() para
    // tambem rejeitar valores absurdamente grandes.
    bool isFinite() const {
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
    }
    bool isSane(float limit = 1.0e7f) const {
        return isFinite() &&
            std::fabs(x) <= limit && std::fabs(y) <= limit && std::fabs(z) <= limit;
    }
};

inline Vec3 operator*(float s, const Vec3& v) { return v * s; }

// ---------------------------------------------------------------------
//  Quat
// ---------------------------------------------------------------------
struct Quat {
    float x, y, z, w;

    Quat() : x(0.f), y(0.f), z(0.f), w(1.f) {}
    Quat(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}

    float len2() const { return x * x + y * y + z * z + w * w; }
    float len()  const { return std::sqrt(len2()); }

    Quat  conjugate() const { return Quat(-x, -y, -z, w); }

    Quat& normalize() {
        const float l = len();
        if (l > 1.0e-8f) { x /= l; y /= l; z /= l; w /= l; }
        return *this;
    }

    bool isFinite() const {
        return std::isfinite(x) && std::isfinite(y) &&
            std::isfinite(z) && std::isfinite(w);
    }
};