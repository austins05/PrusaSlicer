#ifndef slic3r_SequentialCollision_hpp_
#define slic3r_SequentialCollision_hpp_

#include "libslic3r/Point.hpp"

#include <optional>
#include <string>

namespace Slic3r {

struct SequentialCollisionInfo {
	std::string hit_object;
	std::string printing_object;
	std::optional<Vec2d> point;
};

} // namespace Slic3r

#endif
