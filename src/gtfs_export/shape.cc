#include "shape.h"

#include <cmath>
#include <algorithm>
#include <format>
#include <optional>

#include "nigiri/shapes_storage.h"
#include "nigiri/timetable.h"

namespace nigiri::gtfs_export {

shape_slice resolve_shape(timetable const& tt,
                          shapes_storage const& storage,
                          transport_idx_t const transport,
                          trip_idx_t const trip,
                          interval<stop_idx_t> const stops) {
  auto result = shape_slice{};
  if (trip >= storage.trip_offset_indices_.size() ||
      trip >= tt.trip_transport_ranges_.size()) {
    return result;
  }
  auto const [shape, offset] = storage.trip_offset_indices_[trip];
  if (shape == scoped_shape_idx_t::invalid() &&
      offset == shape_offset_idx_t::invalid()) {
    return result;
  }

  // Offsets are relative to the internal trip, not the exported transport.
  result.status_ = shape_status::kTripRange;
  auto range = std::optional<interval<stop_idx_t>>{};
  for (auto const& [t, r] : tt.trip_transport_ranges_[trip]) {
    if (t == transport && r.from_ <= stops.from_ && stops.to_ <= r.to_) {
      if (range.has_value() && *range != r) {
        return result;
      }
      range = r;
    }
  }
  if (!range.has_value() || stops.size() < 2) {
    return result;
  }

  result.status_ = shape_status::kOffsets;
  if (offset == shape_offset_idx_t::invalid() ||
      offset >= storage.offsets_.size()) {
    return result;
  }
  auto const points = storage.get_shape(shape);
  auto const offsets = storage.offsets_[offset];
  if (points.empty() ||
      offsets.size() != static_cast<std::size_t>(range->size())) {
    return result;
  }
  for (auto i = 0U; i != offsets.size(); ++i) {
    if (to_idx(offsets[i]) >= points.size() ||
        (i != 0U && offsets[i] < offsets[i - 1U])) {
      return result;
    }
  }
  auto const from_stop = stops.from_ - range->from_;
  auto const to_stop = stops.to_ - range->from_;
  auto const from = to_idx(offsets[from_stop]);
  auto const to = to_idx(offsets[to_stop - 1U]);
  result.points_ = points.subspan(from, to - from + 1U);
  result.status_ = shape_status::kGeometry;
  auto distance = 0.0;
  for (auto i = 0U; i != result.points_.size(); ++i) {
    auto const p = result.points_[i];
    if (!std::isfinite(p.lat_) || !std::isfinite(p.lng_) ||
        std::abs(p.lat_) > 90.0 || std::abs(p.lng_) > 180.0) {
      return result;
    }
    if (i != 0U) {
      distance += geo::distance(result.points_[i - 1U], p);
    }
    result.distances_.push_back(distance);
  }
  if (!std::isfinite(distance) || distance <= 0.0) {
    return result;
  }
  for (auto s = from_stop; s != to_stop; ++s) {
    result.stop_distances_.push_back(
        result.distances_[to_idx(offsets[s]) - from]);
  }
  // Stored offsets can map consecutive stops to the same point or to
  // duplicate coordinates. GTFS stop distances must strictly increase.
  if (std::adjacent_find(begin(result.stop_distances_),
                         end(result.stop_distances_), [](double a, double b) {
                           return a >= b;
                         }) != end(result.stop_distances_)) {
    result.stop_distances_.clear();
  }
  result.source_ = shape;
  result.id_ = std::format("{}_{}_{}", to_idx(shape), from, to);
  result.status_ = shape_status::kValid;
  return result;
}

}  // namespace nigiri::gtfs_export
