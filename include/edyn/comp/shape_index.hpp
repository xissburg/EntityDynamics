#ifndef EDYN_COMP_SHAPE_INDEX_HPP
#define EDYN_COMP_SHAPE_INDEX_HPP

#include <cstdint>

namespace edyn {

/**
 * @brief Stores the index of the type of shape held by the entity which has
 * this component. The actual shape can be obtained using `visit_shape`.
 */
struct shape_index {
    size_t value;
};

}

#endif // EDYN_COMP_SHAPE_INDEX_HPP
