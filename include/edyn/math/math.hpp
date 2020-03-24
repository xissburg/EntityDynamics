#ifndef EDYN_MATH_MATH_HPP
#define EDYN_MATH_MATH_HPP

#include "constants.hpp"
#include <algorithm>

namespace edyn {

/**
 * @return Value of `radians` converted to degrees.
 */
inline scalar to_degrees(scalar radians) {
    return radians / pi * 180;
}

/**
 * @return Value of `degress` converted to radians.
 */
inline scalar to_radians(scalar degrees) {
    return degrees / 180 * pi;
}

/**
 * @return Scalar clamped to the [0, 1] interval.
 */
inline scalar clamp_unit(scalar s) {
    return std::clamp(s, scalar(0), scalar(1));
}

/**
 * @return Angle in [-π, π].
 */
inline scalar normalize_angle(scalar s) {
    s = std::fmod(s, pi2);

    if (s < -pi) {
        return s + pi2;
    } else if (s > pi) {
        return s - pi2;
    }

    return s;
}

/**
 * @return Linear interpolation between `a` and `b` by scalar `s`.
 */
template<typename T, typename Scalar>
inline auto lerp(T a, T b, Scalar s) {
    return a * (Scalar(1) - s) + b * s;
}

}

#endif // EDYN_MATH_MATH_HPP