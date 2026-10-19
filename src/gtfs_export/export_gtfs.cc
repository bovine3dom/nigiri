#include "nigiri/export_gtfs.h"

#include <cctype>
#include <charconv>
#include <chrono>
#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>

#include "shape.h"

#include "miniz.h"

#include "utl/verify.h"

#include "nigiri/special_stations.h"
#include "nigiri/timetable.h"

namespace nigiri {

namespace {

struct progress_timer {
  explicit progress_timer(std::string label)
      : label_{std::move(label)}, start_{std::chrono::steady_clock::now()} {
    std::cout << "writing " << label_ << " ... " << std::flush;
  }
  progress_timer(progress_timer const&) = delete;
  progress_timer(progress_timer&&) = delete;
  progress_timer& operator=(progress_timer const&) = delete;
  progress_timer& operator=(progress_timer&&) = delete;
  ~progress_timer() {
    auto const elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_)
            .count();
    std::cout << "done. (" << elapsed_ms << "ms)\n";
  }

  std::string label_;
  std::chrono::steady_clock::time_point start_;
};

// Some agencies do not have a name or / and an url
constexpr auto const kDummyAgencyName = std::string_view{"Unknown Agency"};
constexpr auto const kDummyAgencyUrl = std::string_view{"https://example.com"};

}  // namespace

gtfs_dir_export_target::gtfs_dir_export_target(std::filesystem::path dir)
    : dir_{std::move(dir)} {
  std::filesystem::create_directories(dir_);
  utl::verify(std::filesystem::is_empty(dir_),
              "gtfs export: output directory must be empty: {}", dir_.string());
}

std::ostream& gtfs_dir_export_target::create_file(std::string const& filename) {
  streams_.push_back(std::make_unique<std::ofstream>(dir_ / filename));
  utl::verify(streams_.back()->is_open(), "gtfs export: cannot create {}",
              (dir_ / filename).string());
  streams_.back()->exceptions(std::ios::badbit | std::ios::failbit);
  return *streams_.back();
}

void gtfs_dir_export_target::finalize() {
  for (auto& s : streams_) {
    s->flush();
    s->close();
  }
}

namespace {
std::filesystem::path make_tmp_export_dir(
    std::filesystem::path const& zip_path) {
  auto dir = zip_path;
  dir += ".tmp_export";
  if (!dir.parent_path().empty()) {
    std::filesystem::create_directories(dir.parent_path());
  }
  utl::verify(std::filesystem::create_directory(dir),
              "gtfs export: temporary directory already exists: {}",
              dir.string());
  return dir;
}
}  // namespace

struct gtfs_zip_export_target::impl {
  explicit impl(std::filesystem::path const& zip_path) {
    auto const ok =
        mz_zip_writer_init_file(&ar_, zip_path.string().c_str(), 0U);
    utl::verify(ok == MZ_TRUE, "gtfs export: cannot create zip archive {}",
                zip_path.string());
  }
  ~impl() {
    if (ar_.m_pState != nullptr) {
      mz_zip_writer_end(&ar_);
    }
  }
  impl(impl const&) = delete;
  impl(impl&&) = delete;
  impl& operator=(impl const&) = delete;
  impl& operator=(impl&&) = delete;

  mz_zip_archive ar_{};
};

gtfs_zip_export_target::gtfs_zip_export_target(std::filesystem::path zip_path)
    : zip_path_{std::move(zip_path)}, tmp_dir_{make_tmp_export_dir(zip_path_)} {
  try {
    impl_ = std::make_unique<impl>(tmp_dir_ / "feed.zip");
  } catch (...) {
    auto ec = std::error_code{};
    std::filesystem::remove_all(tmp_dir_, ec);
    throw;
  }
}

gtfs_zip_export_target::~gtfs_zip_export_target() {
  streams_.clear();
  impl_.reset();
  auto ec = std::error_code{};
  std::filesystem::remove_all(tmp_dir_, ec);
}

std::ostream& gtfs_zip_export_target::create_file(std::string const& filename) {
  filenames_.push_back(filename);
  streams_.push_back(std::make_unique<std::ofstream>(tmp_dir_ / filename));
  utl::verify(streams_.back()->is_open(), "gtfs export: cannot create {}",
              (tmp_dir_ / filename).string());
  streams_.back()->exceptions(std::ios::badbit | std::ios::failbit);
  return *streams_.back();
}

void gtfs_zip_export_target::finalize() {
  for (auto& s : streams_) {
    s->flush();
    s->close();
  }

  for (auto const& name : filenames_) {
    auto const src = tmp_dir_ / name;
    auto const ok =
        mz_zip_writer_add_file(&impl_->ar_, name.c_str(), src.string().c_str(),
                               nullptr, 0, MZ_BEST_SPEED);
    utl::verify(ok == MZ_TRUE, "gtfs export: cannot add {} to zip archive {}",
                name, zip_path_.string());
  }

  auto const ok = mz_zip_writer_finalize_archive(&impl_->ar_);
  utl::verify(ok == MZ_TRUE, "gtfs export: cannot finalize zip archive {}",
              zip_path_.string());
  utl::verify(mz_zip_writer_end(&impl_->ar_) == MZ_TRUE,
              "gtfs export: cannot close zip archive {}", zip_path_.string());
  impl_.reset();
  std::filesystem::rename(tmp_dir_ / "feed.zip", zip_path_);
}

std::unique_ptr<gtfs_export_target> make_gtfs_export_target(
    std::filesystem::path const& output_path) {
  auto ext = output_path.extension().string();
  std::ranges::transform(ext, ext.begin(), [](unsigned char const c) {
    return static_cast<char>(std::tolower(c));
  });
  if (ext == ".zip") {
    return std::make_unique<gtfs_zip_export_target>(output_path);
  }
  return std::make_unique<gtfs_dir_export_target>(output_path);
}

namespace {

// RFC 4180: fields containing a separator, a quote or a line break are
// enclosed in double quotes, quotes inside are escaped by doubling them.
// Line breaks are replaced by spaces since many GTFS consumers read files
// line by line.
std::string csv_escape(std::string_view input) {
  if (input.find_first_of(",\"\n\r") == std::string_view::npos) {
    return std::string{input};
  }

  auto out = std::string{};
  out.reserve(input.size() + 2U);
  out.push_back('"');
  for (auto const c : input) {
    if (c == '"') {
      out.append("\"\"");
    } else if (c == '\n' || c == '\r') {
      out.push_back(' ');
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

// GTFS times are relative to "noon minus 12h" of the service day. Since the
// export uses Etc/UTC (no DST), this is midnight of the service day, which
// matches nigiri's internal representation (minutes after UTC midnight of
// the day the bitfield refers to). Values >= 24:00:00 are valid GTFS.
std::string format_time(delta const d) {
  auto const total_minutes =
      std::chrono::duration_cast<std::chrono::minutes>(d.as_duration()).count();
  return std::format("{:02}:{:02}:00", total_minutes / 60, total_minutes % 60);
}

std::string format_coord(double const x) {
  // 8 decimals ~ 1mm. Default ostream precision (6 significant digits) is
  // only ~10m, shortest round-trip repr. exposes float noise.
  auto str = std::format("{:.8f}", x);
  str.erase(str.find_last_not_of('0') + 1U);
  if (str.back() == '.') {
    str.pop_back();
  }
  return str;
}

std::size_t stop_id(location_idx_t const l) {
  return to_idx(l) - kNSpecialStations;
}

// Stations (location_type=1) must not be referenced in stop_times.txt.
// Some data sources (e.g. HRD) serve the parent location directly. For those,
// the location itself is exported as a regular stop (keeping its ID) and an
// additional station entry is generated to act as parent.
std::string station_id(location_idx_t const l, bool const served) {
  return served ? std::format("{}_station", stop_id(l))
                : std::to_string(stop_id(l));
}

bool section_flag(timetable const& tt,
                  route_idx_t const r,
                  route_flag const flag,
                  stop_idx_t const section) {
  auto const flags = tt.route_flags_per_section_[flag][r];
  return flags.empty() ? tt.is_flag_set(flag, r)
                       : flags.at(flags.size() == 1U ? 0U : section);
}

struct exported_trip {
  transport_idx_t transport_;
  trip_idx_t trip_;
  interval<stop_idx_t> stops_;
  bool split_;

  std::string id() const {
    return split_ ? std::format("{}_{}", to_idx(transport_), stops_.from_)
                  : std::to_string(to_idx(transport_));
  }
};

template <typename Fn>
void for_each_exported_trip(timetable const& tt, Fn&& fn) {
  for (auto t = transport_idx_t{0}; t < tt.transport_traffic_days_.size();
       ++t) {
    if (tt.bitfields_[tt.transport_traffic_days_[t]].none()) {
      continue;
    }
    auto const r = tt.transport_route_[t];
    auto const n_stops =
        static_cast<stop_idx_t>(tt.route_location_seq_[r].size());
    auto const sections = tt.transport_to_trip_section_[t];
    auto const trip_at = [&](stop_idx_t const section) {
      return tt.merged_trips_[sections.at(sections.size() == 1U ? 0U : section)]
          .front();
    };
    auto const same_flags = [&](stop_idx_t const a, stop_idx_t const b) {
      for (auto const flag :
           {kBikesAllowed, kCarsAllowed, kWheelchairAccessible}) {
        if (section_flag(tt, r, flag, a) != section_flag(tt, r, flag, b)) {
          return false;
        }
      }
      return true;
    };
    auto from = stop_idx_t{0};
    auto const emit = [&](stop_idx_t const to) {
      fn(exported_trip{
          t, trip_at(from), {from, to}, from != 0U || to != n_stops});
    };
    for (auto s = stop_idx_t{1}; s < n_stops - 1U; ++s) {
      if (trip_at(from) != trip_at(s) || !same_flags(from, s)) {
        // Adjacent trips share the boundary stop.
        emit(static_cast<stop_idx_t>(s + 1U));
        from = s;
      }
    }
    emit(n_stops);
  }
}

gtfs_export::shape_slice shape_for(timetable const& tt,
                                   shapes_storage const* shapes,
                                   exported_trip const& trip) {
  return shapes == nullptr
             ? gtfs_export::shape_slice{}
             : gtfs_export::resolve_shape(tt, *shapes, trip.transport_,
                                          trip.trip_, trip.stops_);
}

std::string format_decimal(double const value) {
  // Fixed notation, with the shortest representation that round-trips.
  // Some GTFS consumers do not support scientific notation.
  char buffer[512];
  auto const [end, ec] = std::to_chars(std::begin(buffer), std::end(buffer),
                                       value, std::chars_format::fixed);
  utl::verify(ec == std::errc{}, "Cannot format shape value {}", value);
  return {buffer, end};
}

void write_shapes(timetable const& tt,
                  gtfs_export_target& target,
                  shapes_storage const& shapes) {
  auto const timer = progress_timer{"shapes.txt"};
  auto& out = target.create_file("shapes.txt");
  out << "shape_id,shape_pt_lat,shape_pt_lon,shape_pt_sequence,"
         "shape_dist_traveled\n";
  auto seen = std::unordered_set<std::string>{};
  using enum gtfs_export::shape_status;
  auto counts = std::map<gtfs_export::shape_status, std::size_t>{};
  auto publisher = std::size_t{0};
  auto routed = std::size_t{0};
  auto without_distances = std::size_t{0};
  for_each_exported_trip(tt, [&](exported_trip const& trip) {
    auto const shape = shape_for(tt, &shapes, trip);
    ++counts[shape.status_];
    if (shape.status_ != kValid) {
      return;
    }
    ++(get_shape_source(shape.source_) == shape_source::kRouted ? routed
                                                                : publisher);
    without_distances += shape.stop_distances_.empty() ? 1U : 0U;
    if (!seen.emplace(shape.id_).second) {
      return;
    }
    auto seq = 0U;
    for (auto i = 0U; i != shape.points_.size(); ++i) {
      // Keep distances strictly increasing without changing stop offsets.
      if (i != 0U && shape.distances_[i] == shape.distances_[i - 1U]) {
        continue;
      }
      auto const p = shape.points_[i];
      out << std::format("{},{},{},{},{}\n", shape.id_, format_decimal(p.lat_),
                         format_decimal(p.lng_), seq++,
                         format_decimal(shape.distances_[i]));
    }
  });
  std::cout << std::format(
      "trips: {} publisher, {} routed, {} missing; omitted: {} trip range, "
      "{} offsets, {} geometry; {} shapes, {} trips without stop distances. ",
      publisher, routed, counts[kMissing], counts[kTripRange], counts[kOffsets],
      counts[kGeometry], seen.size(), without_distances);
}

void write_feed_info(gtfs_export_target& out_target) {
  auto const timer = progress_timer{"feed_info.txt"};

  auto& out = out_target.create_file("feed_info.txt");
  out << "feed_publisher_name,feed_publisher_url,feed_lang\n";
  out << "MOTIS Export,https://github.com/motis-project/nigiri,en\n";
}

void write_agencies(timetable const& tt, gtfs_export_target& out_target) {
  auto const timer = progress_timer{"agency.txt"};

  auto& out = out_target.create_file("agency.txt");
  out << "agency_id,agency_name,agency_url,agency_timezone\n";

  for (auto p = provider_idx_t{0}; p < tt.providers_.size(); ++p) {
    auto const& provider = tt.providers_[p];

    auto const raw_name = tt.get_default_translation(provider.name_);
    auto const raw_url = tt.get_default_translation(provider.url_);

    auto const name =
        std::string_view{raw_name.empty() ? kDummyAgencyName : raw_name};
    auto const url =
        std::string_view{raw_url.empty() ? kDummyAgencyUrl : raw_url};

    // All times are exported in UTC. GTFS requires all agencies to share the
    // same timezone.
    out << to_idx(p) << "," << csv_escape(name) << "," << csv_escape(url)
        << ",Etc/UTC\n";
  }
}

void write_stops(timetable const& tt, gtfs_export_target& out_target) {
  auto const timer = progress_timer{"stops.txt"};

  auto& out = out_target.create_file("stops.txt");
  out << "stop_id,original_stop_id,stop_code,stop_name,stop_desc,stop_lat,"
         "stop_lon,location_type,parent_station,platform_code\n";

  // GTFS only supports one level of hierarchy (stop -> station).
  // Nested hierarchies are flattened by attaching every stop to its root.
  auto is_parent = std::vector<bool>(tt.n_locations(), false);
  for (auto l = location_idx_t{kNSpecialStations}; l < tt.n_locations(); ++l) {
    auto const root = tt.locations_.get_root_idx(l);
    if (root != l) {
      is_parent[to_idx(root)] = true;
    }
  }

  auto is_served = std::vector<bool>(tt.n_locations(), false);
  for (auto r = route_idx_t{0}; r < tt.n_routes(); ++r) {
    for (auto const s : tt.route_location_seq_[r]) {
      is_served[to_idx(stop{s}.location_idx())] = true;
    }
  }

  auto const write_row = [&](location_idx_t const l, std::string_view id,
                             int const location_type, std::string_view parent) {
    auto const coord = tt.locations_.coordinates_[l];
    // stop_name is required: fall back to the root's name, then the stop id.
    auto name = tt.get_default_name(l);
    if (name.empty()) {
      name = tt.get_default_name(tt.locations_.get_root_idx(l));
    }
    if (name.empty()) {
      name = tt.locations_.ids_[l].view();
    }
    out << id << "," << csv_escape(tt.locations_.ids_[l].view()) << ","
        << csv_escape(tt.get_default_translation(tt.locations_.stop_codes_[l]))
        << "," << csv_escape(name) << ","
        << csv_escape(
               tt.get_default_translation(tt.locations_.descriptions_[l]))
        << "," << format_coord(coord.lat_) << "," << format_coord(coord.lng_)
        << "," << location_type << "," << parent << ","
        << csv_escape(location_type == 1
                          ? ""
                          : tt.get_default_translation(
                                tt.locations_.platform_codes_[l]))
        << "\n";
  };

  for (auto l = location_idx_t{kNSpecialStations}; l < tt.n_locations(); ++l) {
    if (is_parent[to_idx(l)]) {
      write_row(l, station_id(l, is_served[to_idx(l)]), 1, "");
    }
  }

  for (auto l = location_idx_t{kNSpecialStations}; l < tt.n_locations(); ++l) {
    auto const root = tt.locations_.get_root_idx(l);
    if (root != l) {
      write_row(l, std::to_string(stop_id(l)), 0,
                station_id(root, is_served[to_idx(root)]));
    } else if (!is_parent[to_idx(l)]) {
      write_row(l, std::to_string(stop_id(l)), 0, "");
    } else if (is_served[to_idx(l)]) {
      write_row(l, std::to_string(stop_id(l)), 0, station_id(l, true));
    }
  }
}

void write_stop_times(timetable const& tt,
                      gtfs_export_target& out_target,
                      shapes_storage const* shapes) {
  auto const timer = progress_timer{"stop_times.txt"};

  auto& out = out_target.create_file("stop_times.txt");
  out << "trip_id,arrival_time,departure_time,stop_id,stop_sequence,"
         "stop_headsign,pickup_type,drop_off_type";
  out << (shapes != nullptr ? ",shape_dist_traveled\n" : "\n");

  for_each_exported_trip(tt, [&](exported_trip const& trip) {
    auto const shape = shape_for(tt, shapes, trip);
    auto const t = trip.transport_;
    auto const stops = tt.route_location_seq_[tt.transport_route_[t]];
    auto const last = static_cast<stop_idx_t>(trip.stops_.to_ - 1U);
    auto const id = trip.id();
    auto const directions = tt.transport_section_directions_[t];
    for (auto s = trip.stops_.from_; s < trip.stops_.to_; ++s) {
      auto const stp = stop{stops[s]};
      auto const arr = tt.event_mam(
          t, s, s == trip.stops_.from_ ? event_type::kDep : event_type::kArr);
      auto const dep =
          tt.event_mam(t, s, s == last ? event_type::kArr : event_type::kDep);
      auto const headsign = directions.size() > 1U
                                ? tt.get_default_translation(
                                      directions.at(s == last ? s - 1U : s))
                                : std::string_view{};
      out << id << "," << format_time(arr) << "," << format_time(dep) << ","
          << stop_id(stp.location_idx()) << "," << (s - trip.stops_.from_)
          << "," << csv_escape(headsign) << "," << (stp.in_allowed() ? 0 : 1)
          << "," << (stp.out_allowed() ? 0 : 1);
      if (shapes != nullptr) {
        out << ",";
        if (shape.status_ == gtfs_export::shape_status::kValid &&
            !shape.stop_distances_.empty()) {
          out << format_decimal(shape.stop_distances_[s - trip.stops_.from_]);
        }
      }
      out << "\n";
    }
  });
}

void write_trips(timetable const& tt,
                 gtfs_export_target& out_target,
                 std::vector<size_t> const& route_offsets,
                 shapes_storage const* shapes) {
  auto const timer = progress_timer{"trips.txt"};

  auto& out = out_target.create_file("trips.txt");
  out << "route_id,service_id,trip_id,trip_headsign,trip_short_name,"
         "wheelchair_accessible,bikes_allowed,cars_allowed,block_id";
  out << (shapes != nullptr ? ",shape_id\n" : "\n");

  for_each_exported_trip(tt, [&](exported_trip const& trip) {
    auto const t = trip.transport_;
    auto const r = tt.transport_route_[t];
    // Nigiri does not distinguish "not allowed" from "no information".
    auto const flag = [&](route_flag const f) {
      return section_flag(tt, r, f, trip.stops_.from_) ? 1 : 0;
    };
    auto const source_id = tt.trip_id_src_[tt.trip_ids_[trip.trip_].front()];
    auto const route_id = tt.trip_route_id_[trip.trip_];
    auto const directions = tt.transport_section_directions_[t];
    auto const headsign =
        directions.empty()
            ? std::string_view{}
            : tt.get_default_translation(directions.at(
                  directions.size() == 1U ? 0U : trip.stops_.from_));
    out << (route_offsets[to_idx(source_id)] + to_idx(route_id)) << ","
        << to_idx(tt.transport_traffic_days_[t]) << "," << trip.id() << ","
        << csv_escape(headsign) << ","
        << csv_escape(
               tt.get_default_translation(tt.trip_short_names_[trip.trip_]))
        << "," << flag(kWheelchairAccessible) << "," << flag(kBikesAllowed)
        << "," << flag(kCarsAllowed) << ","
        << (trip.split_ ? std::to_string(to_idx(t)) : "");
    if (shapes != nullptr) {
      auto const shape = shape_for(tt, shapes, trip);
      out << ","
          << (shape.status_ == gtfs_export::shape_status::kValid ? shape.id_
                                                                 : "");
    }
    out << "\n";
  });
}

void write_routes(timetable const& tt,
                  gtfs_export_target& out_target,
                  std::vector<size_t> const& route_offsets) {
  auto const timer = progress_timer{"routes.txt"};

  auto& out = out_target.create_file("routes.txt");

  auto const to_global_route_id = [&](source_idx_t s, route_id_idx_t r) {
    return route_offsets[to_idx(s)] + to_idx(r);
  };

  out << "route_id,agency_id,route_short_name,route_long_name,route_type,"
         "route_color,route_text_color\n";

  // Only export routes referenced by at least one exported trip.
  auto used = std::vector<bool>(
      route_offsets.empty() ? 0U
                            : route_offsets.back() +
                                  tt.route_ids_.back().ids_.size(),
      false);
  for_each_exported_trip(tt, [&](exported_trip const& trip) {
    auto const source_id = tt.trip_id_src_[tt.trip_ids_[trip.trip_].front()];
    used[to_global_route_id(source_id, tt.trip_route_id_[trip.trip_])] = true;
  });

  for (auto s = source_idx_t{0}; s < tt.route_ids_.size(); ++s) {
    auto const& routes = tt.route_ids_[s];
    auto const n_route_ids = static_cast<route_id_idx_t>(routes.ids_.size());
    for (auto r = route_id_idx_t{0}; r < n_route_ids; ++r) {
      auto const global_id = to_global_route_id(s, r);
      if (!used[global_id]) {
        continue;
      }
      auto const short_name =
          tt.get_default_translation(routes.route_id_short_names_[r]);
      auto long_name =
          tt.get_default_translation(routes.route_id_long_names_[r]);
      // GTFS requires route_short_name or route_long_name.
      auto fallback = std::string{};
      if (short_name.empty() && long_name.empty()) {
        long_name = routes.ids_.get(r);
        if (long_name.empty()) {
          fallback = std::to_string(global_id);
          long_name = fallback;
        }
      }
      auto const agency = to_idx(routes.route_id_provider_[r]);
      auto const type = to_idx(routes.route_id_type_[r]);
      auto const& rc = routes.route_id_colors_[r];
      auto const color_str = to_str(rc.color_).value_or("");
      auto const text_str = to_str(rc.text_color_).value_or("");

      out << global_id << "," << agency << "," << csv_escape(short_name) << ","
          << csv_escape(long_name) << "," << type << "," << color_str << ","
          << text_str << "\n";
    }
  }
}

void write_transfers(timetable const& tt, gtfs_export_target& out_target) {
  auto const timer = progress_timer{"transfers.txt"};

  auto& out = out_target.create_file("transfers.txt");
  out << "from_stop_id,to_stop_id,transfer_type,min_transfer_time\n";

  for (auto l = location_idx_t{kNSpecialStations}; l < tt.n_locations(); ++l) {
    // from_stop_id == to_stop_id: minimum transfer time at this stop
    auto const transfer_time = std::chrono::duration_cast<std::chrono::seconds>(
                                   tt.locations_.transfer_time_[l])
                                   .count();
    out << stop_id(l) << "," << stop_id(l) << ",2," << transfer_time << "\n";

    for (auto const& fp : tt.locations_.footpaths_out_[0][l]) {
      out << stop_id(l) << "," << stop_id(fp.target()) << ",2,"
          << (fp.duration().count() * 60) << "\n";
    }
  }
}

void write_calendar(timetable const& tt, gtfs_export_target& out_target) {
  auto const timer = progress_timer{"calendar.txt and calendar_dates.txt"};

  auto& cal = out_target.create_file("calendar.txt");
  auto& exc = out_target.create_file("calendar_dates.txt");

  cal << "service_id,monday,tuesday,wednesday,thursday,friday,saturday,sunday,"
         "start_date,end_date\n";
  exc << "service_id,date,exception_type\n";

  auto const& base = tt.internal_interval_days().from_;

  auto const to_ymd_str = [&](std::size_t day) {
    auto const sys_day = base + date::days{static_cast<int>(day)};
    auto const ymd = date::year_month_day{sys_day};
    return std::format("{:04}{:02}{:02}", int(ymd.year()),
                       unsigned(ymd.month()), unsigned(ymd.day()));
  };

  auto const get_weekday = [&](std::size_t day) -> unsigned {
    auto const sys_day = base + date::days{static_cast<int>(day)};
    return static_cast<unsigned>(date::weekday{sys_day}.c_encoding());
  };

  auto const write_as_exceptions_only =
      [&](bitfield const& bf, std::size_t const first, std::size_t const last,
          bitfield_idx_t const b) {
        for (auto d = first; d <= last; ++d) {
          if (bf.test(d)) {
            exc << to_idx(b) << "," << to_ymd_str(d) << ",1\n";
          }
        }
      };

  for (auto b = bitfield_idx_t{0}; b < tt.bitfields_.size(); ++b) {
    auto const& bf = tt.bitfields_[b];
    if (bf.none()) {
      continue;
    }

    auto first = std::size_t{0};
    auto last = std::size_t{0};
    for (auto d = std::size_t{0}; d < bf.size(); ++d) {
      if (bf.test(d)) {
        first = d;
        break;
      }
    }
    for (auto d = bf.size(); d-- > 0;) {
      if (bf.test(d)) {
        last = d;
        break;
      }
    }

    if (last - first + 1 < 7) {
      write_as_exceptions_only(bf, first, last, b);
      continue;
    }

    auto active_count = std::array<int, 7>{};
    auto total_count = std::array<int, 7>{};
    for (auto d = first; d <= last; ++d) {
      auto const wd = get_weekday(d);
      ++total_count[wd];
      active_count[wd] += static_cast<int>(bf.test(d));
    }

    auto weekly_pattern = std::uint8_t{0};
    for (auto wd = 0U; wd < 7U; ++wd) {
      if (active_count[wd] * 2 > total_count[wd]) {
        weekly_pattern |= static_cast<std::uint8_t>(1U << wd);
      }
    }

    if (weekly_pattern == 0) {
      write_as_exceptions_only(bf, first, last, b);
      continue;
    }

    cal << to_idx(b) << ","  //
        << ((weekly_pattern >> 1U) & 1U) << ","  // Monday
        << ((weekly_pattern >> 2U) & 1U) << ","  // Tuesday
        << ((weekly_pattern >> 3U) & 1U) << ","  // Wednesday
        << ((weekly_pattern >> 4U) & 1U) << ","  // Thursday
        << ((weekly_pattern >> 5U) & 1U) << ","  // Friday
        << ((weekly_pattern >> 6U) & 1U) << ","  // Saturday
        << ((weekly_pattern >> 0U) & 1U) << ","  // Sunday
        << to_ymd_str(first) << "," << to_ymd_str(last) << "\n";

    for (auto d = first; d <= last; ++d) {
      auto const active = bf.test(d);
      auto const in_pattern = ((weekly_pattern >> get_weekday(d)) & 1U) != 0U;

      if (active && !in_pattern) {
        exc << to_idx(b) << "," << to_ymd_str(d) << ",1\n";
      } else if (!active && in_pattern) {
        exc << to_idx(b) << "," << to_ymd_str(d) << ",2\n";
      }
    }
  }
}
}  // namespace

void export_gtfs(timetable const& tt,
                 std::filesystem::path const& output_path,
                 shapes_storage const* shapes) {
  auto target = make_gtfs_export_target(output_path);
  auto route_offsets = std::vector<std::size_t>(tt.route_ids_.size());
  auto sum = std::size_t{0};
  for (auto s = source_idx_t{0}; s < tt.route_ids_.size(); ++s) {
    route_offsets[to_idx(s)] = sum;
    sum += tt.route_ids_[s].ids_.size();
  }

  write_feed_info(*target);
  write_agencies(tt, *target);
  write_stops(tt, *target);
  write_routes(tt, *target, route_offsets);
  write_trips(tt, *target, route_offsets, shapes);
  write_stop_times(tt, *target, shapes);
  if (shapes != nullptr) {
    write_shapes(tt, *target, *shapes);
  }
  write_calendar(tt, *target);
  write_transfers(tt, *target);
  target->finalize();
}

}  // namespace nigiri
