#pragma once

#include <span>
#include <string>
#include <vector>

#include "geo/latlng.h"

#include "nigiri/types.h"

namespace nigiri {
struct shapes_storage;
struct timetable;

namespace gtfs_export {

enum class shape_status { kValid, kMissing, kTripRange, kOffsets, kGeometry };

struct shape_slice {
  shape_status status_{shape_status::kMissing};
  scoped_shape_idx_t source_{scoped_shape_idx_t::invalid()};
  std::string id_;
  std::span<geo::latlng const> points_;
  std::vector<double> distances_;
  // Empty if the stored offsets do not give strictly increasing distances.
  std::vector<double> stop_distances_;
};

shape_slice resolve_shape(timetable const&,
                          shapes_storage const&,
                          transport_idx_t,
                          trip_idx_t,
                          interval<stop_idx_t>);

}  // namespace gtfs_export
}  // namespace nigiri
