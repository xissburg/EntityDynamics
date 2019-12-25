#include "edyn/collision/collide.hpp"

namespace edyn {

collision_result collide(const sphere_shape &sphere, const vector3 &posA, const quaternion &ornA,
                         const plane_shape &plane, const vector3 &posB, const quaternion &ornB,
                         scalar threshold) {
    auto normal = rotate(ornB, plane.normal);
    auto center = posB + rotate(ornB, plane.normal * plane.constant);
    auto d = posA - center;
    auto l = dot(normal, d);

    if (l > sphere.radius) {
        return {};
    }

    auto result = collision_result {};
    result.num_points = 1;
    result.point[0].pivotA = rotate(conjugate(ornA), -normal * sphere.radius);
    result.point[0].pivotB = rotate(conjugate(ornB), d - normal * l - center);
    result.point[0].normalB = plane.normal;
    result.point[0].distance = l - sphere.radius;
    return result;
}

}