#ifndef HOST_CAMERA_PARAMS_H
#define HOST_CAMERA_PARAMS_H

// engine/set/camera parameter validation.
//
// The handler used to pass JsonToVec3 results straight to Engine::SetCamera,
// and JsonToVec3 maps a missing or malformed field to (0,0,0). A coincident
// eye/target, a zero or view-parallel up vector, or a non-finite component all
// make D3DXMatrixLookAtRH normalize a zero vector: the view matrix fills with
// NaN, the scene vanishes, and the next wheel-zoom divides by a zero distance.
// Now the request is refused with ok:false and the engine camera is untouched.
//
// Pure (no D3D, no engine) so tests/test_camera_params.cpp can pin it.

#include <cfloat>
#include <cmath>

#include "third_party/nlohmann/json.hpp"

namespace host {

struct CameraParams
{
    float position[3];
    float target[3];
    float up[3];
};

namespace detail {

// One `[x, y, z]` field: an array of at least three finite numbers (extra
// elements are ignored, as JsonToVec3 always did).
inline bool ReadCameraVec3(const nlohmann::json& params, const char* name, float out[3])
{
    const auto it = params.find(name);
    if (it == params.end() || !it->is_array() || it->size() < 3) return false;
    for (int i = 0; i < 3; ++i)
    {
        const nlohmann::json& v = (*it)[i];
        if (!v.is_number()) return false;
        // Range-check in double first: a double outside float's range would
        // otherwise narrow to inf (or UB) rather than be refused.
        const double d = v.get<double>();
        if (!std::isfinite(d) || std::fabs(d) > FLT_MAX) return false;
        out[i] = static_cast<float>(d);
    }
    return true;
}

} // namespace detail

// Fills `out` and returns nullptr when the params describe a usable camera;
// otherwise returns the reason (the handler's ok:false error text).
inline const char* ReadCameraParams(const nlohmann::json& params, CameraParams& out)
{
    if (!params.is_object()
        || !detail::ReadCameraVec3(params, "position", out.position)
        || !detail::ReadCameraVec3(params, "target",   out.target)
        || !detail::ReadCameraVec3(params, "up",       out.up))
    {
        return "camera position, target and up must each be [x, y, z] finite numbers";
    }

    // In double: squares of large finite floats overflow float.
    double dir[3], up[3];
    for (int i = 0; i < 3; ++i)
    {
        dir[i] = static_cast<double>(out.target[i]) - out.position[i];
        up[i]  = out.up[i];
    }
    const double dirLen = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    const double upLen  = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    if (!(dirLen > 1e-4))
        return "camera position and target coincide";

    // |up x dir| = |up| |dir| sin(angle). Reject only a (numerically) exactly
    // zero or parallel up; near-pole orbit poses stay legal.
    const double cx = up[1] * dir[2] - up[2] * dir[1];
    const double cy = up[2] * dir[0] - up[0] * dir[2];
    const double cz = up[0] * dir[1] - up[1] * dir[0];
    const double crossLen = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (!(upLen > 0.0) || !(crossLen > 1e-6 * upLen * dirLen))
        return "camera up is zero or parallel to the view direction";

    return nullptr;
}

} // namespace host

#endif  // HOST_CAMERA_PARAMS_H
