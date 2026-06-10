// Copyright 2025, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
// Copied from DisplayXR SDK reference example (parallax_toggle)

#pragma once

#include <math.h>
#include <DirectXMath.h>

#pragma pack(push,1)

namespace leia {

struct vec3f
{
    union
    {
        struct { float x;         ///< X component
                 float y;         ///< Y component
                 float z; };      ///< Z component
        struct { float e[3]; };   ///< Indexed components
    };

    // Default constructor
    vec3f() = default;

    // Constructors
    explicit vec3f (float xyz) : x(xyz), y(xyz), z(xyz) {}
    explicit vec3f (float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

    // Binary operators
    friend vec3f operator+ (const vec3f& lhs, float rhs)        { return vec3f(lhs.x + rhs, lhs.y + rhs, lhs.z + rhs); }
    friend vec3f operator- (const vec3f& lhs, float rhs)        { return vec3f(lhs.x - rhs, lhs.y - rhs, lhs.z - rhs); }
    friend vec3f operator* (const vec3f& lhs, float rhs)        { return vec3f(lhs.x * rhs, lhs.y * rhs, lhs.z * rhs); }
    friend vec3f operator/ (const vec3f& lhs, float rhs)        { const float InvRHS = 1.0f / rhs; return vec3f(lhs.x * InvRHS, lhs.y * InvRHS, lhs.z * InvRHS); }
    friend vec3f operator+ (float lhs, const vec3f& rhs)        { return vec3f(lhs + rhs.x, lhs + rhs.y, lhs + rhs.z); }
    friend vec3f operator- (float lhs, const vec3f& rhs)        { return vec3f(lhs - rhs.x, lhs - rhs.y, lhs - rhs.z); }
    friend vec3f operator* (float lhs, const vec3f& rhs)        { return vec3f(lhs * rhs.x, lhs * rhs.y, lhs * rhs.z); }
    friend vec3f operator/ (float lhs, const vec3f& rhs)        { return vec3f(lhs / rhs.x, lhs / rhs.y, lhs / rhs.z); }
    friend vec3f operator+ (const vec3f& lhs, const vec3f& rhs) { return vec3f(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z); }
    friend vec3f operator- (const vec3f& lhs, const vec3f& rhs) { return vec3f(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z); }
    friend vec3f operator* (const vec3f& lhs, const vec3f& rhs) { return vec3f(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z); }
    friend vec3f operator/ (const vec3f& lhs, const vec3f& rhs) { return vec3f(lhs.x / rhs.x, lhs.y / rhs.y, lhs.z / rhs.z); }

    // Singular operators
    float  operator[](int index) const        { return e[index]; }
    float& operator[](int index)              { return e[index]; }
    bool   operator==(const vec3f& rhs) const { return (x == rhs.x) && (y == rhs.y) && (z == rhs.z); }
    bool   operator!=(const vec3f& rhs) const { return (x != rhs.x) || (y != rhs.y) || (z != rhs.z); }
    vec3f& operator+=(const vec3f& rhs)       { x += rhs.x; y += rhs.y; z += rhs.z; return *this; }
    vec3f& operator-=(const vec3f& rhs)       { x -= rhs.x; y -= rhs.y; z -= rhs.z; return *this; }
    vec3f& operator*=(const vec3f& rhs)       { x *= rhs.x; y *= rhs.y; z *= rhs.z; return *this; }
    vec3f& operator/=(const vec3f& rhs)       { x /= rhs.x; y /= rhs.y; z /= rhs.z; return *this; }
    vec3f& operator+=(float rhs)              { x += rhs; y += rhs; z += rhs; return *this; }
    vec3f& operator-=(float rhs)              { x -= rhs; y -= rhs; z -= rhs; return *this; }
    vec3f& operator*=(float rhs)              { x *= rhs; y *= rhs; z *= rhs; return *this; }
    vec3f& operator/=(float rhs)              { const float InvRHS = 1.0f / rhs;  x *= InvRHS; y *= InvRHS; z *= InvRHS; return *this; }

    // Unary operators
    vec3f operator-() const { return vec3f(-x, -y, -z); };

    // Static methods
    static vec3f cross (const vec3f& lhs, const vec3f& rhs) { return vec3f(lhs.y * rhs.z - lhs.z * rhs.y, lhs.z * rhs.x - lhs.x * rhs.z, lhs.x * rhs.y - lhs.y * rhs.x); }
    static float dot   (const vec3f& lhs, const vec3f& rhs) { return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z); }
};

struct vec4f
{
    union
    {
        struct { float x;         ///< X component
                 float y;         ///< Y component
                 float z;         ///< Z component
                 float w; };      ///< W component
        struct { float e[4]; };   ///< Indexed components
    };

    // Default constructor
    vec4f() = default;

    // Constructors
    explicit vec4f(float xyzw) : x(xyzw), y(xyzw), z(xyzw), w(xyzw) {}
    explicit vec4f(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}

    // Binary operators
    friend vec4f operator+(const vec4f& lhs, float rhs)        { return vec4f(lhs.x + rhs, lhs.y + rhs, lhs.z + rhs, lhs.w + rhs); };
    friend vec4f operator-(const vec4f& lhs, float rhs)        { return vec4f(lhs.x - rhs, lhs.y - rhs, lhs.z - rhs, lhs.w - rhs); };
    friend vec4f operator*(const vec4f& lhs, float rhs)        { return vec4f(lhs.x * rhs, lhs.y * rhs, lhs.z * rhs, lhs.w * rhs); };
    friend vec4f operator/(const vec4f& lhs, float rhs)        { const float InvRHS = 1.0f / rhs; return vec4f(lhs.x * InvRHS, lhs.y * InvRHS, lhs.z * InvRHS, lhs.w * InvRHS); };
    friend vec4f operator+(float lhs, const vec4f& rhs)        { return vec4f(lhs + rhs.x, lhs + rhs.y, lhs + rhs.z, lhs + rhs.w); };
    friend vec4f operator-(float lhs, const vec4f& rhs)        { return vec4f(lhs - rhs.x, lhs - rhs.y, lhs - rhs.z, lhs - rhs.w); };
    friend vec4f operator*(float lhs, const vec4f& rhs)        { return vec4f(lhs * rhs.x, lhs * rhs.y, lhs * rhs.z, lhs * rhs.w); };
    friend vec4f operator/(float lhs, const vec4f& rhs)        { return vec4f(lhs / rhs.x, lhs / rhs.y, lhs / rhs.z, lhs / rhs.w); };
    friend vec4f operator+(const vec4f& lhs, const vec4f& rhs) { return vec4f(lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z, lhs.w + rhs.w); };
    friend vec4f operator-(const vec4f& lhs, const vec4f& rhs) { return vec4f(lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z, lhs.w - rhs.w); };
    friend vec4f operator*(const vec4f& lhs, const vec4f& rhs) { return vec4f(lhs.x * rhs.x, lhs.y * rhs.y, lhs.z * rhs.z, lhs.w * rhs.w); };
    friend vec4f operator/(const vec4f& lhs, const vec4f& rhs) { return vec4f(lhs.x / rhs.x, lhs.y / rhs.y, lhs.z / rhs.z, lhs.w / rhs.w); };

    // Singular operators
    float  operator[](int index) const        { return e[index]; }
    float& operator[](int index)              { return e[index]; }
    bool   operator==(const vec4f& rhs) const { return (x == rhs.x) && (y == rhs.y) && (z == rhs.z) && (w == rhs.w); }
    bool   operator!=(const vec4f& rhs) const { return (x != rhs.x) || (y != rhs.y) || (z != rhs.z) || (w != rhs.w); }
    vec4f& operator+=(const vec4f& rhs)       { x += rhs.x; y += rhs.y; z += rhs.z; w += rhs.w; return *this; }
    vec4f& operator-=(const vec4f& rhs)       { x -= rhs.x; y -= rhs.y; z -= rhs.z; w -= rhs.w; return *this; }
    vec4f& operator*=(const vec4f& rhs)       { x *= rhs.x; y *= rhs.y; z *= rhs.z; w *= rhs.w; return *this; }
    vec4f& operator/=(const vec4f& rhs)       { x /= rhs.x; y /= rhs.y; z /= rhs.z; w /= rhs.w; return *this; }
    vec4f& operator+=(float rhs)              { x += rhs; y += rhs; z += rhs; w += rhs; return *this; }
    vec4f& operator-=(float rhs)              { x -= rhs; y -= rhs; z -= rhs; w -= rhs; return *this; }
    vec4f& operator*=(float rhs)              { x *= rhs; y *= rhs; z *= rhs; w *= rhs; return *this; }
    vec4f& operator/=(float rhs)              { const float InvRHS = 1.0f / rhs; x *= InvRHS; y *= InvRHS; z *= InvRHS; w *= InvRHS; return *this; }

    // Unary operators
    vec4f operator-() const { return vec4f(-x, -y, -z, -w); };
};

struct mat4f
{
    union
    {
        struct { vec4f x;                   ///< X-Axis
                 vec4f y;                   ///< Y-Axis
                 vec4f z;                   ///< Z-Axis
                 vec4f w; };                ///< W-Axis
        struct { float Xx, Xy, Xz, Xw;      ///< X-Axis scalars
                 float Yx, Yy, Yz, Yw;      ///< Y-Axis scalars
                 float Zx, Zy, Zz, Zw;      ///< Z-Axis scalars
                 float Wx, Wy, Wz, Ww; };   ///< W-Axis scalars
        struct { float M[4][4]; };          ///< All components in a float array
        struct { float m[16]; };            ///< All components in an array
        struct { vec4f e[4]; };             ///< Indexed axes
    };

    mat4f() = default;

    mat4f
    (
        float _Xx, float _Xy, float _Xz, float _Xw,
        float _Yx, float _Yy, float _Yz, float _Yw,
        float _Zx, float _Zy, float _Zz, float _Zw,
        float _Wx, float _Wy, float _Wz, float _Ww)
    {
        Xx = _Xx; Xy = _Xy; Xz = _Xz; Xw = _Xw;
        Yx = _Yx; Yy = _Yy; Yz = _Yz; Yw = _Yw;
        Zx = _Zx; Zy = _Zy; Zz = _Zz; Zw = _Zw;
        Wx = _Wx; Wy = _Wy; Wz = _Wz; Ww = _Ww;
    }

    vec4f  operator[](int index) const { return e[index]; }
    vec4f& operator[](int index)       { return e[index]; }

    mat4f operator*(const mat4f& rhs) const
    {
        mat4f ret;
        ret.x = x * rhs.Xx + y * rhs.Xy + z * rhs.Xz + w * rhs.Xw;
        ret.y = x * rhs.Yx + y * rhs.Yy + z * rhs.Yz + w * rhs.Yw;
        ret.z = x * rhs.Zx + y * rhs.Zy + z * rhs.Zz + w * rhs.Zw;
        ret.w = x * rhs.Wx + y * rhs.Wy + z * rhs.Wz + w * rhs.Ww;
        return ret;
    }

    static mat4f identity()
    {
        mat4f m = {};
        m.x = vec4f(1.0f, 0.0f, 0.0f, 0.0f);
        m.y = vec4f(0.0f, 1.0f, 0.0f, 0.0f);
        m.z = vec4f(0.0f, 0.0f, 1.0f, 0.0f);
        m.w = vec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return m;
    }

    static mat4f perspective(float left, float right, float bottom, float top, float nearVal, float farVal)
    {
        mat4f m = {};
        m.M[0][0] = (2.0f * nearVal) / (right - left);
        m.M[1][1] = (2.0f * nearVal) / (top - bottom);
        m.M[2][0] = (right + left) / (right - left);
        m.M[2][1] = (top + bottom) / (top - bottom);
        m.M[2][2] = -(farVal + nearVal) / (farVal - nearVal);
        m.M[2][3] = -1.0f;
        m.M[3][2] = -(2.0f * farVal * nearVal) / (farVal - nearVal);
        return m;
    }

    static mat4f translation(float tx, float ty, float tz)
    {
        mat4f m = {};
        m.x = vec4f(1.0f, 0.0f, 0.0f, 0.0f);
        m.y = vec4f(0.0f, 1.0f, 0.0f, 0.0f);
        m.z = vec4f(0.0f, 0.0f, 1.0f, 0.0f);
        m.w = vec4f(tx,   ty,   tz,   1.0f);
        return m;
    }

    static mat4f translation(const vec3f& t)
    {
        return translation(t.x, t.y, t.z);
    }

    static mat4f scaling(float sx, float sy, float sz)
    {
        mat4f m = {};
        m.x = vec4f(sx,   0.0f, 0.0f, 0.0f);
        m.y = vec4f(0.0f, sy,   0.0f, 0.0f);
        m.z = vec4f(0.0f, 0.0f, sz,   0.0f);
        m.w = vec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return m;
    }

    static mat4f rotationY(float angle)
    {
        const float ca = cosf(angle);
        const float sa = sinf(angle);

        mat4f m = {};
        m.x = vec4f(ca, 0.0f, -sa, 0.0f);
        m.y = vec4f(0.0f, 1.0f, 0.0f, 0.0f);
        m.z = vec4f(sa, 0.0f, ca, 0.0f);
        m.w = vec4f(0.0f, 0.0f, 0.0f, 1.0f);
        return m;
    }
};

// Kooima off-axis projection matrix calculation
// Based on Robert Kooima's paper "Generalized Perspective Projection"
// Screen is assumed to be at Z=0 plane, centered at origin
// All units in millimeters
inline mat4f CalculateMVP(const vec3f& eye, float screenWidthMM, float screenHeightMM,
                           float cubeRotation, float cubeSize, float znear, float zfar)
{
    // Model matrix: rotation * scaling * translation(0,0,0)
    mat4f model = {};
    {
        const mat4f translation = mat4f::translation(0.0f, 0.0f, 0.0f);
        const mat4f scaling     = mat4f::scaling(cubeSize, cubeSize, cubeSize);
        const mat4f rotation    = mat4f::rotationY(cubeRotation);
        model = rotation * scaling * translation;
    }

    // View matrix: identity (camera is at eye position via projection)
    mat4f view = mat4f::identity();

    // Projection matrix: Kooima off-axis projection
    mat4f projection = {};
    {
        // Screen corners in world space (screen at Z=0, centered at origin)
        vec3f pa = vec3f(-screenWidthMM / 2.0f,  screenHeightMM / 2.0f, 0.0f);  // top-left
        vec3f pb = vec3f( screenWidthMM / 2.0f,  screenHeightMM / 2.0f, 0.0f);  // top-right
        vec3f pc = vec3f(-screenWidthMM / 2.0f, -screenHeightMM / 2.0f, 0.0f);  // bottom-left

        // Screen coordinate system (screen faces +Z direction)
        vec3f vr = vec3f(1.0f, 0.0f, 0.0f);  // right
        vec3f vu = vec3f(0.0f, 1.0f, 0.0f);  // up
        vec3f vn = vec3f(0.0f, 0.0f, 1.0f);  // normal (toward viewer)

        // Vectors from eye to screen corners
        vec3f va = pa - eye;
        vec3f vb = pb - eye;
        vec3f vc = pc - eye;

        // Distance from eye to screen plane
        float distance = -vec3f::dot(va, vn);

        // Frustum parameters at near plane
        float l = vec3f::dot(vr, va) * znear / distance;
        float r = vec3f::dot(vr, vb) * znear / distance;
        float b = vec3f::dot(vu, vc) * znear / distance;
        float t = vec3f::dot(vu, va) * znear / distance;

        // Build projection matrix
        mat4f frustum = mat4f::perspective(l, r, b, t, znear, zfar);
        mat4f translate = mat4f::translation(-eye);
        projection = frustum * translate;
    }

    // Final MVP matrix
    mat4f mvp = projection * view * model;
    return mvp;
}

} // namespace leia

#pragma pack(pop)
