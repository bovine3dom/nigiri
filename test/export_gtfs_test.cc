#include "gtest/gtest.h"

#include <chrono>
#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include "nigiri/loader/dir.h"
#include "nigiri/loader/gtfs/load_timetable.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/loader/netex/load_timetable.h"
#include "nigiri/export_gtfs.h"
#include "nigiri/rt/frun.h"
#include "nigiri/shapes_storage.h"
#include "nigiri/special_stations.h"

#include "nigiri/timetable.h"
#include "../src/gtfs_export/shape.h"

#include "test_dir.h"

using namespace nigiri;
using namespace date;
using namespace std::string_view_literals;

namespace {

// - P is a parent station with platforms P1, P2
// - T2 serves the parent station P directly (-> needs a generated station)
// - names/headsigns/descriptions contain quotes, commas
// - T1 runs past midnight, has pickup/drop-off restrictions
constexpr auto const kFeed = R"(
# agency.txt
agency_id,agency_name,agency_url,agency_timezone
DB,"Deutsche Bahn, ""DB""",https://deutschebahn.com,Europe/Berlin

# stops.txt
stop_id,stop_name,stop_desc,stop_lat,stop_lon,location_type,parent_station,platform_code
P,Hauptbahnhof,,49.8728253,8.6511929,1,,
P1,"Hauptbahnhof, Gleis 1","Desc, with ""quotes""",49.8729123,8.6512456,0,P,1
P2,Hauptbahnhof Gleis 2,,49.8727001,8.6510002,0,P,2
A,"Stop ""A""",,49.9123456,8.7123456,0,,
B,B,,50.0,8.8,0,,

# routes.txt
route_id,agency_id,route_short_name,route_long_name,route_type,route_color,route_text_color
R1,DB,"S1, Express","Long ""Name"", with comma",109,FF0000,FFFFFF
R2,DB,Bus 2,,3,,

# trips.txt
route_id,service_id,trip_id,trip_headsign,trip_short_name
R1,WD,T1,"To ""B""",123
R2,ALL,T2,A,

# stop_times.txt
trip_id,arrival_time,departure_time,stop_id,stop_sequence,pickup_type,drop_off_type,stop_headsign
T1,10:00:00,10:00:00,P1,0,0,1,
T1,10:30:00,10:31:00,A,1,1,0,
T1,26:30:00,26:30:00,B,2,0,0,
T2,12:00:00,12:00:00,P,0,0,0,Via A
T2,12:10:00,12:10:00,A,1,0,0,Final
T2,12:20:00,12:20:00,B,2,0,0,

# calendar.txt
service_id,monday,tuesday,wednesday,thursday,friday,saturday,sunday,start_date,end_date
WD,1,1,1,1,1,0,0,20190501,20190531
ALL,1,1,1,1,1,1,1,20190501,20190531

# calendar_dates.txt
service_id,date,exception_type
WD,20190530,2
WD,20190504,1
)"sv;

constexpr auto kDateRange =
    interval{sys_days{2019_y / May / 1}, sys_days{2019_y / June / 1}};

timetable load(loader::dir const& d,
               interval<sys_days> const dates = kDateRange,
               shapes_storage* shapes = nullptr) {
  auto tt = timetable{};
  tt.date_range_ = dates;
  loader::register_special_stations(tt);
  loader::gtfs::load_timetable({}, source_idx_t{0}, d, tt, nullptr, shapes);
  loader::finalize(tt);
  return tt;
}

void add_shared_shape(timetable const& tt,
                      shapes_storage& storage,
                      std::vector<geo::latlng> const& points,
                      std::vector<shape_offset_t> const& offsets = {
                          shape_offset_t{0}, shape_offset_t{1},
                          shape_offset_t{2}}) {
  auto const shape = to_scoped_shape_idx(shape_idx_t{storage.data_.size()},
                                         shape_source::kTimetable);
  storage.data_.emplace_back(points);
  auto const offset = storage.add_offsets(offsets);
  for (auto trip = trip_idx_t{0}; trip != tt.n_trips(); ++trip) {
    storage.add_trip_shape_offsets(trip, {shape, offset});
  }
}

void expect_empty_last_column(loader::dir const& dir, char const* file) {
  auto const data = dir.get_file(file);
  auto lines = std::istringstream{std::string{data.data()}};
  auto line = std::string{};
  std::getline(lines, line);
  while (std::getline(lines, line)) {
    ASSERT_FALSE(line.empty());
    EXPECT_EQ(',', line.back());
  }
}

std::string_view root_name(timetable const& tt, location_idx_t const l) {
  return tt.get_default_name(tt.locations_.get_root_idx(l));
}

// Compare supported trip fields without IDs, including duplicate events.
std::multiset<std::string> trip_events(timetable const& tt) {
  auto ret = std::multiset<std::string>{};
  for (auto t = transport_idx_t{0}; t < tt.transport_traffic_days_.size();
       ++t) {
    auto const& bf = tt.bitfields_[tt.transport_traffic_days_[t]];
    auto const r = tt.transport_route_[t];
    auto const stops = tt.route_location_seq_[r];
    auto const directions = tt.transport_section_directions_[t];
    for (auto d = 0U; d < bf.size(); ++d) {
      if (!bf.test(d)) {
        continue;
      }
      auto s = std::stringstream{};
      auto const fr = rt::frun::from_t(tt, nullptr, transport{t, day_idx_t{d}});
      for (auto i = stop_idx_t{0}; i != stops.size(); ++i) {
        if (i != stops.size() - 1U) {
          auto const rs = fr[i];
          auto const [routes, route] = rs.get_route(event_type::kDep);
          s << tt.get_default_translation(
                   rs.get_provider(event_type::kDep).name_)
            << " | "
            << tt.get_default_translation(routes->route_id_short_names_[route])
            << " | "
            << tt.get_default_translation(routes->route_id_long_names_[route])
            << " | " << routes->route_id_type_[route] << " | "
            << to_str(routes->route_id_colors_[route].color_).value_or("-")
            << " | "
            << tt.get_default_translation(
                   tt.trip_short_names_[rs.get_trip_idx(event_type::kDep)])
            << " bikes=" << rs.is_flag_set(kBikesAllowed, event_type::kDep)
            << " cars=" << rs.is_flag_set(kCarsAllowed, event_type::kDep)
            << " wheelchair="
            << rs.is_flag_set(kWheelchairAccessible, event_type::kDep) << "\n";
        }
        auto const stp = stop{stops[i]};
        auto const l = stp.location_idx();
        auto const tr = transport{t, day_idx_t{d}};
        s << "  " << tt.get_default_name(l) << " [" << root_name(tt, l)
          << "] in=" << stp.in_allowed() << " out=" << stp.out_allowed();
        if (i != 0U) {
          s << " arr=" << tt.event_time(tr, i, event_type::kArr);
        }
        if (i != stops.size() - 1U) {
          s << " dep=" << tt.event_time(tr, i, event_type::kDep) << " dir="
            << (directions.empty()
                    ? ""
                    : tt.get_default_translation(
                          directions[directions.size() == 1U ? 0U : i]));
        }
        s << "\n";
      }
      ret.emplace(s.str());
    }
  }
  return ret;
}

std::set<std::string> stops(timetable const& tt) {
  auto ret = std::set<std::string>{};
  for (auto l = location_idx_t{kNSpecialStations}; l < tt.n_locations(); ++l) {
    auto const c = tt.locations_.coordinates_[l];
    ret.emplace(std::format(
        "{} [{}] ({}, {}) desc={} platform={}", tt.get_default_name(l),
        root_name(tt, l), c.lat_, c.lng_,
        tt.get_default_translation(tt.locations_.descriptions_[l]),
        tt.get_default_translation(tt.locations_.platform_codes_[l])));
  }
  return ret;
}

struct export_gtfs_test : testing::TestWithParam<std::tuple<bool, unsigned>> {
  bool is_zip() const { return std::get<0>(GetParam()); }

  void export_gtfs(timetable const& tt,
                   std::filesystem::path const& path,
                   shapes_storage const* shapes = nullptr) const {
    nigiri::export_gtfs(tt, path, shapes, {std::get<1>(GetParam()), 1});
  }

  void SetUp() override {
    auto const root =
        std::filesystem::temp_directory_path() /
        std::format(
            "nigiri_export_{}",
            std::chrono::steady_clock::now().time_since_epoch().count());
    ASSERT_TRUE(std::filesystem::create_directory(root));
    root_ = root;
  }

  void TearDown() override {
    if (!root_.empty()) {
      std::filesystem::remove_all(root_);
    }
  }

  std::filesystem::path output() const {
    return root_ / (is_zip() ? "feed.ZIP" : "feed");
  }

  std::filesystem::path root_;
};

}  // namespace

TEST_P(export_gtfs_test, shape_slices) {
  auto const tt = load(loader::mem_dir::read(kFeed));
  auto storage =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  auto const points = std::vector<geo::latlng>{
      {49.0, 8.0}, {50.0, 8.0}, {50.0, 8.0}, {51.0, 8.0}, {49.0, 8.0}};
  storage.data_.emplace_back(points);
  storage.routed_data_.emplace_back(points);
  auto const offsets = storage.add_offsets(
      {shape_offset_t{0}, shape_offset_t{2}, shape_offset_t{4}});
  auto const trip = trip_idx_t{0};
  auto const [transport, range] = tt.trip_transport_ranges_[trip].front();
  ASSERT_EQ(3U, range.size());
  auto const publisher =
      to_scoped_shape_idx(shape_idx_t{0}, shape_source::kTimetable);
  auto const routed =
      to_scoped_shape_idx(shape_idx_t{0}, shape_source::kRouted);
  storage.add_trip_shape_offsets(trip, {publisher, offsets});
  auto resolve = [&]() {
    return gtfs_export::resolve_shape(tt, storage, transport, trip, range);
  };
  auto const full = resolve();
  ASSERT_EQ(gtfs_export::shape_status::kValid, full.status_);
  EXPECT_EQ(5U, full.points_.size());
  EXPECT_EQ(3U, full.stop_distances_.size());
  EXPECT_EQ(0.0, full.stop_distances_.front());
  EXPECT_GT(full.stop_distances_.back(), full.stop_distances_[1]);
  EXPECT_EQ(full.distances_[1], full.distances_[2]);
  auto const clipped = gtfs_export::resolve_shape(
      tt, storage, transport, trip,
      {static_cast<stop_idx_t>(range.from_ + 1U), range.to_});
  ASSERT_EQ(gtfs_export::shape_status::kValid, clipped.status_);
  EXPECT_EQ(3U, clipped.points_.size());
  EXPECT_EQ(0.0, clipped.stop_distances_.front());
  EXPECT_NE(full.id_, clipped.id_);
  storage.trip_offset_indices_[trip].first = routed;
  EXPECT_NE(full.id_, resolve().id_);
  for (auto const& equal :
       {std::vector<shape_offset_t>{shape_offset_t{0}, shape_offset_t{0},
                                    shape_offset_t{4}},
        std::vector<shape_offset_t>{shape_offset_t{1}, shape_offset_t{2},
                                    shape_offset_t{4}}}) {
    storage.trip_offset_indices_[trip].second = storage.add_offsets(equal);
    auto const slice = resolve();
    EXPECT_EQ(gtfs_export::shape_status::kValid, slice.status_);
    EXPECT_FALSE(slice.points_.empty());
    EXPECT_TRUE(slice.stop_distances_.empty());
  }
  storage.trip_offset_indices_[trip].second = storage.add_offsets(
      {shape_offset_t{0}, shape_offset_t{4}, shape_offset_t{2}});
  EXPECT_EQ(gtfs_export::shape_status::kOffsets, resolve().status_);
  storage.trip_offset_indices_[trip].second = storage.add_offsets(
      {shape_offset_t{0}, shape_offset_t{2}, shape_offset_t{5}});
  EXPECT_EQ(gtfs_export::shape_status::kOffsets, resolve().status_);
  storage.trip_offset_indices_[trip].second = shape_offset_idx_t::invalid();
  EXPECT_EQ(gtfs_export::shape_status::kOffsets, resolve().status_);
  storage.trip_offset_indices_[trip].first = scoped_shape_idx_t::invalid();
  EXPECT_EQ(gtfs_export::shape_status::kMissing, resolve().status_);
  storage.trip_offset_indices_[trip] = {publisher, offsets};
  storage.data_[shape_idx_t{0}][2].lat_ =
      std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(gtfs_export::shape_status::kGeometry, resolve().status_);
  for (auto& p : storage.data_[shape_idx_t{0}]) {
    p = points.front();
  }
  EXPECT_EQ(gtfs_export::shape_status::kGeometry, resolve().status_);
  EXPECT_EQ(gtfs_export::shape_status::kTripRange,
            gtfs_export::resolve_shape(tt, storage, transport_idx_t::invalid(),
                                       trip, range)
                .status_);
}

TEST_P(export_gtfs_test, equal_stop_distances) {
  auto const tt = load(loader::mem_dir::read(kFeed));
  auto shapes =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  add_shared_shape(tt, shapes, {{49.0, 8.0}, {49.0, 8.0}, {50.0, 8.0}});
  export_gtfs(tt, output(), &shapes);
  auto const exported = loader::make_dir(output());
  EXPECT_EQ(trip_events(tt), trip_events(load(*exported)));
  EXPECT_NE(std::string_view::npos,
            exported->get_file("trips.txt").data().find(",0_0_2\n"));
  expect_empty_last_column(*exported, "stop_times.txt");
}

TEST_P(export_gtfs_test, small_shape_values) {
  auto const tt = load(loader::mem_dir::read(kFeed));
  auto shapes =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  auto const points =
      std::vector<geo::latlng>{{0.0, 0.0}, {1e-12, 2e-12}, {2e-12, 4e-12}};
  add_shared_shape(tt, shapes, points);
  export_gtfs(tt, output(), &shapes);
  auto const exported = loader::make_dir(output());
  auto const data = exported->get_file("shapes.txt");
  auto const rows = data.data().substr(data.data().find('\n') + 1U);
  EXPECT_EQ(std::string_view::npos, rows.find_first_of("eE"));
  auto imported_shapes =
      shapes_storage{root_ / "imported-shapes", cista::mmap::protection::WRITE};
  auto const imported = load(*exported, kDateRange, &imported_shapes);
  ASSERT_EQ(1U, imported_shapes.data_.size());
  auto const actual = imported_shapes.data_[shape_idx_t{0}];
  ASSERT_EQ(points.size(), actual.size());
  for (auto i = 0U; i != points.size(); ++i) {
    EXPECT_DOUBLE_EQ(points[i].lat_, actual[i].lat_);
    EXPECT_DOUBLE_EQ(points[i].lng_, actual[i].lng_);
  }
  for (auto const& [shape, offset] : imported_shapes.trip_offset_indices_) {
    auto const actual_offsets = imported_shapes.offsets_[offset];
    ASSERT_EQ(3U, actual_offsets.size());
    for (auto i = 0U; i != 3U; ++i) {
      EXPECT_EQ(shape_offset_t{i}, actual_offsets[i]);
    }
  }
}

TEST_P(export_gtfs_test, shapes_export) {
  auto const tt = load(loader::mem_dir::read(kFeed));
  auto storage =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  auto const points = std::vector<geo::latlng>{
      {49.0, 8.0}, {50.0, 8.0}, {50.0, 8.0}, {51.0, 8.0}, {49.0, 8.0}};
  add_shared_shape(tt, storage, points,
                   {shape_offset_t{0}, shape_offset_t{2}, shape_offset_t{4}});
  storage.routed_data_.emplace_back(points);
  for (auto const mode : {"shared"sv, "routed"sv, "invalid"sv}) {
    SCOPED_TRACE(mode);
    if (mode == "routed") {
      storage.trip_offset_indices_[trip_idx_t{1}].first =
          to_scoped_shape_idx(shape_idx_t{0}, shape_source::kRouted);
    } else if (mode == "invalid") {
      for (auto& entry : storage.trip_offset_indices_) {
        entry.second = shape_offset_idx_t::invalid();
      }
    }
    auto const dir = root_ / mode;
    auto const path = dir / output().filename();
    export_gtfs(tt, path, &storage);
    auto const exported = loader::make_dir(path);
    EXPECT_EQ(trip_events(tt), trip_events(load(*exported)));
    auto expected = std::string{
        "shape_id,shape_pt_lat,shape_pt_lon,shape_pt_sequence,"
        "shape_dist_traveled\n"};
    if (mode != "invalid") {
      auto const d = geo::distance(points[0], points[1]);
      for (auto source : {shape_source::kTimetable, shape_source::kRouted}) {
        if (source == shape_source::kRouted && mode == "shared") {
          continue;
        }
        auto const id = std::format(
            "{}_0_4", to_idx(to_scoped_shape_idx(shape_idx_t{0}, source)));
        expected += std::format(
            "{0},49,8,0,0\n{0},50,8,1,{1}\n"
            "{0},51,8,2,{2}\n{0},49,8,3,{3}\n",
            id, d, d + geo::distance(points[2], points[3]),
            d + geo::distance(points[2], points[3]) +
                geo::distance(points[3], points[4]));
      }
    }
    EXPECT_EQ(expected, exported->get_file("shapes.txt").data());
    if (mode != "invalid") {
      auto imported_shapes = shapes_storage{dir / "imported-shapes",
                                            cista::mmap::protection::WRITE};
      auto const imported = load(*exported, kDateRange, &imported_shapes);
      ASSERT_EQ(tt.n_trips(), imported.n_trips());
      for (auto trip = trip_idx_t{0}; trip != imported.n_trips(); ++trip) {
        auto const shape = imported_shapes.get_shape(trip);
        ASSERT_EQ(4U, shape.size());
        EXPECT_EQ(points.front(), shape.front());
        EXPECT_EQ(points.back(), shape.back());
        auto const [index, offset] = imported_shapes.trip_offset_indices_[trip];
        auto const stops = imported_shapes.offsets_[offset];
        ASSERT_EQ(3U, stops.size());
        EXPECT_EQ(shape_offset_t{0}, stops[0]);
        EXPECT_EQ(shape_offset_t{1}, stops[1]);
        EXPECT_EQ(shape_offset_t{3}, stops[2]);
      }
    }
    if (mode == "invalid") {
      expect_empty_last_column(*exported, "trips.txt");
      expect_empty_last_column(*exported, "stop_times.txt");
    }
  }
}

TEST_P(export_gtfs_test, round_trip) {
  auto const tt = load(loader::mem_dir::read(kFeed));

  // loader: quoted fields are unescaped
  auto const p1 = tt.find(location_id{"P1", source_idx_t{0}});
  ASSERT_TRUE(p1.has_value());
  EXPECT_EQ(R"(Desc, with "quotes")",
            tt.get_default_translation(tt.locations_.descriptions_[*p1]));
  EXPECT_EQ(
      R"(Long "Name", with comma)",
      tt.get_default_translation(tt.route_ids_[source_idx_t{0}]
                                     .route_id_long_names_[route_id_idx_t{0}]));

  auto const tt_path = root_ / "tt.bin";
  tt.write(tt_path);
  auto stored = timetable::read(tt_path);
  stored->resolve();
  export_gtfs(*stored, output());

  auto const tt2 = load(*loader::make_dir(output()));

  auto const expected_events = trip_events(tt);
  EXPECT_FALSE(expected_events.empty());
  EXPECT_EQ(expected_events, trip_events(tt2));

  EXPECT_EQ(stops(tt), stops(tt2));

  // The served parent P gets an additional generated station entry.
  EXPECT_EQ(tt.n_locations() + 1U, tt2.n_locations());

  // Served locations must never be stations (location_type=1).
  for (auto r = route_idx_t{0}; r < tt2.n_routes(); ++r) {
    for (auto const s : tt2.route_location_seq_[r]) {
      EXPECT_TRUE(tt2.locations_.children_[stop{s}.location_idx()].empty());
    }
  }
}

TEST_P(export_gtfs_test, utc_service_dates) {
  using namespace std::chrono_literals;
  struct test_case {
    std::string_view zone_;
    sys_days day_;
    std::chrono::minutes utc_offset_;
  };
  auto index = 0U;
  for (auto const& c :
       {test_case{"Europe/Berlin", 2019_y / March / 30, -60min},
        test_case{"Europe/Berlin", 2019_y / March / 31, -120min},
        test_case{"Europe/Berlin", 2019_y / April / 1, -120min},
        test_case{"Europe/Berlin", 2019_y / October / 26, -120min},
        test_case{"Europe/Berlin", 2019_y / October / 27, -60min},
        test_case{"Europe/Berlin", 2019_y / October / 28, -60min},
        test_case{"Asia/Tokyo", 2019_y / January / 1, -540min},
        test_case{"America/Los_Angeles", 2019_y / December / 31, 480min}}) {
    SCOPED_TRACE(std::format("{} {}", c.zone_, date::format("%F", c.day_)));
    auto feed = loader::mem_dir::read(kFeed);
    feed.dir_["agency.txt"] = std::format(
        "agency_id,agency_name,agency_url,agency_timezone\n"
        "DB,Agency,https://example.com,{}\n",
        c.zone_);
    feed.dir_["trips.txt"] = "route_id,service_id,trip_id\nR1,S,T\n";
    feed.dir_["stop_times.txt"] =
        "trip_id,arrival_time,departure_time,stop_id,stop_sequence\n"
        "T,00:30:00,00:30:00,A,1\nT,26:30:00,26:30:00,B,2\n";
    feed.dir_.erase("calendar.txt");
    feed.dir_["calendar_dates.txt"] =
        std::format("service_id,date,exception_type\nS,{},1\n",
                    date::format("%Y%m%d", c.day_));
    auto const dates = interval{c.day_, c.day_ + days{1}};
    auto const tt = load(feed, dates);
    auto const path = root_ / std::to_string(index++) / output().filename();
    export_gtfs(tt, path);
    auto const tt2 = load(*loader::make_dir(path), dates);
    EXPECT_EQ(trip_events(tt), trip_events(tt2));

    // GTFS times use the UTC offset at local noon, including on DST dates.
    auto const departure = unixtime_t{c.day_ + 30min + c.utc_offset_};
    for (auto const* timetable : {&tt, &tt2}) {
      auto count = 0U;
      for (auto t = transport_idx_t{0};
           t < timetable->transport_traffic_days_.size(); ++t) {
        auto const& bf =
            timetable->bitfields_[timetable->transport_traffic_days_[t]];
        for (auto d = 0U; d < bf.size(); ++d) {
          if (bf.test(d)) {
            ++count;
            auto const tr = transport{t, day_idx_t{d}};
            EXPECT_EQ(departure,
                      timetable->event_time(tr, 0, event_type::kDep));
            EXPECT_EQ(departure + 26h,
                      timetable->event_time(tr, 1, event_type::kArr));
          }
        }
      }
      EXPECT_EQ(1U, count);
    }
  }
}

TEST_P(export_gtfs_test, source_files) {
  auto tt = load(loader::mem_dir::read(kFeed));
  ASSERT_EQ(2U, tt.n_trips());
  auto const names = std::array{"feeds/one.gtfs.zip/stop_times.txt",
                                "feeds/two, \"quoted\".xml"};
  for (auto trip = trip_idx_t{0}; trip != tt.n_trips(); ++trip) {
    tt.trip_debug_[trip].front().source_file_idx_ =
        tt.register_source_file(names[to_idx(trip)]);
  }
  export_gtfs(tt, output());
  EXPECT_EQ(
      "attribution_id,route_id,organization_name,is_producer,attribution_url\n"
      "nigiri:0:0:0,0,feeds/one.gtfs.zip/stop_times.txt,1,\n"
      "nigiri:0:0:1,0,\"feeds/two, \"\"quoted\"\".xml\",1,\n"
      "nigiri:0:1:0,1,feeds/one.gtfs.zip/stop_times.txt,1,\n"
      "nigiri:0:1:1,1,\"feeds/two, \"\"quoted\"\".xml\",1,\n",
      loader::make_dir(output())->get_file("attributions.txt").data());

  for (auto trip = trip_idx_t{0}; trip != tt.n_trips(); ++trip) {
    tt.trip_debug_[trip].clear();
  }
  auto const path = root_ / "missing" / output().filename();
  export_gtfs(tt, path);
  EXPECT_EQ(
      "attribution_id,route_id,organization_name,is_producer,attribution_url\n"
      "nigiri:0:0:0,0,unknown-source-0,1,\n"
      "nigiri:0:1:0,1,unknown-source-0,1,\n",
      loader::make_dir(path)->get_file("attributions.txt").data());
}

TEST_P(export_gtfs_test, colliding_source_ids) {
  auto first = loader::mem_dir::read(kFeed);
  first.dir_["routes.txt"] += "UNUSED,DB,Unused,,3,,\n";
  for (auto const identical : {false, true}) {
    SCOPED_TRACE(identical);
    auto second = first;
    if (!identical) {
      second.dir_["agency.txt"] =
          "agency_id,agency_name,agency_url,agency_timezone\n"
          "DB,Other Agency,https://example.com,Asia/Tokyo\n";
      second.dir_["routes.txt"] =
          "route_id,agency_id,route_short_name,route_long_name,route_type\n"
          "R2,DB,Other Bus,,3\nR1,DB,Other Rail,,2\n";
    }
    auto expected = trip_events(load(first));
    auto const other_events = trip_events(load(second));
    expected.insert(begin(other_events), end(other_events));
    ASSERT_FALSE(other_events.empty());

    auto tt = timetable{};
    tt.date_range_ = kDateRange;
    tt.n_sources_ = 2;
    loader::register_special_stations(tt);
    auto bitfields = hash_map<bitfield, bitfield_idx_t>{};
    loader::gtfs::load_timetable({}, source_idx_t{0}, first, tt, bitfields);
    loader::gtfs::load_timetable({}, source_idx_t{1}, second, tt, bitfields);
    for (auto trip = trip_idx_t{0}; trip != tt.n_trips(); ++trip) {
      auto const source = tt.trip_id_src_[tt.trip_ids_[trip].front()];
      tt.trip_debug_[trip].front().source_file_idx_ = tt.register_source_file(
          to_idx(source) == 0U ? "fr_sncf_voyageurs.gtfs.zip/stop_times.txt"
                               : "be_sncb.gtfs.zip/stop_times.txt");
    }
    loader::finalize(tt);
    EXPECT_EQ(expected, trip_events(tt));

    auto const path =
        root_ / (identical ? "identical" : "different") / output().filename();
    export_gtfs(tt, path);
    auto const exported = loader::make_dir(path);
    EXPECT_EQ(
        "attribution_id,route_id,organization_name,is_producer,attribution_"
        "url\n"
        "nigiri:0:0:0,0,fr_sncf_voyageurs.gtfs.zip/stop_times.txt,1,\n"
        "nigiri:0:1:0,1,fr_sncf_voyageurs.gtfs.zip/stop_times.txt,1,\n"
        "nigiri:1:3:0,3,be_sncb.gtfs.zip/stop_times.txt,1,\n"
        "nigiri:1:4:0,4,be_sncb.gtfs.zip/stop_times.txt,1,\n",
        exported->get_file("attributions.txt").data());
    auto const tt2 = load(*exported);
    EXPECT_EQ(expected, trip_events(tt2));
    EXPECT_EQ(stops(tt), stops(tt2));
  }
}

TEST_P(export_gtfs_test, merged_services) {
  for (auto const partial_overlap : {false, true}) {
    SCOPED_TRACE(partial_overlap);
    auto first = loader::mem_dir::read(kFeed);
    first.dir_["trips.txt"] = "route_id,service_id,trip_id\nR1,S,T\n";
    first.dir_["stop_times.txt"] =
        "trip_id,arrival_time,departure_time,stop_id,stop_sequence\n"
        "T,10:00:00,10:00:00,A,1\nT,10:30:00,10:30:00,B,2\n";
    first.dir_.erase("calendar.txt");
    first.dir_["calendar_dates.txt"] =
        "service_id,date,exception_type\nS,20190501,1\nS,20190502,1\n";
    auto second = first;
    if (partial_overlap) {
      second.dir_["calendar_dates.txt"] =
          "service_id,date,exception_type\nS,20190502,1\nS,20190503,1\n";
    }
    auto const first_events = trip_events(load(first));
    auto const second_events = trip_events(load(second));
    auto unique = std::set<std::string>{begin(first_events), end(first_events)};
    unique.insert(begin(second_events), end(second_events));
    auto const expected =
        std::multiset<std::string>{begin(unique), end(unique)};
    ASSERT_EQ(partial_overlap ? 3U : 2U, expected.size());

    auto tt = timetable{};
    tt.date_range_ = kDateRange;
    tt.n_sources_ = 2;
    loader::register_special_stations(tt);
    auto bitfields = hash_map<bitfield, bitfield_idx_t>{};
    loader::gtfs::load_timetable({}, source_idx_t{0}, first, tt, bitfields);
    loader::gtfs::load_timetable({}, source_idx_t{1}, second, tt, bitfields);
    loader::finalize(tt, false, false, true);
    ASSERT_EQ(expected, trip_events(tt));

    auto const path = root_ / (partial_overlap ? "partial" : "identical") /
                      output().filename();
    export_gtfs(tt, path);
    EXPECT_EQ(expected, trip_events(load(*loader::make_dir(path))));
  }
}

TEST_P(export_gtfs_test, section_flags_without_trip_change) {
  auto tt = load(loader::mem_dir::read(kFeed));
  auto const t = transport_idx_t{0};
  auto const r = tt.transport_route_[t];
  ASSERT_EQ(1U, tt.transport_to_trip_section_[t].size());
  ASSERT_EQ(3U, tt.route_location_seq_[r].size());
  for (auto const flag : {kBikesAllowed, kCarsAllowed, kWheelchairAccessible}) {
    tt.route_flags_[flag].set(to_idx(r) * 2U, false);
    tt.route_flags_[flag].set(to_idx(r) * 2U + 1U, true);
    auto sections = tt.route_flags_per_section_[flag][r];
    ASSERT_TRUE(sections.empty());
    sections.push_back(true);
    sections.push_back(false);
  }
  auto const expected = trip_events(tt);
  export_gtfs(tt, output());
  auto const tt2 = load(*loader::make_dir(output()));
  EXPECT_EQ(3U, tt2.trip_ids_.size());
  EXPECT_EQ(expected, trip_events(tt2));

  auto shapes =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  add_shared_shape(tt, shapes, {{49.0, 8.0}, {50.0, 8.0}, {51.0, 8.0}});
  auto const path = root_ / "shaped" / output().filename();
  export_gtfs(tt, path, &shapes);
  auto const data = loader::make_dir(path)->get_file("trips.txt");
  EXPECT_NE(std::string_view::npos, data.data().find(",0_0_1\n"));
  EXPECT_NE(std::string_view::npos, data.data().find(",0_1_2\n"));
  EXPECT_EQ(expected, trip_events(load(*loader::make_dir(path))));
}

TEST_P(export_gtfs_test, through_service) {
  auto feed = loader::mem_dir::read(kFeed);
  feed.dir_["agency.txt"] =
      "agency_id,agency_name,agency_url,agency_timezone\n"
      "DB,First Agency,https://example.com,Europe/Berlin\n"
      "OTHER,Second Agency,https://example.org,Europe/Berlin\n";
  feed.dir_["routes.txt"] =
      "route_id,agency_id,route_short_name,route_long_name,route_type\n"
      "R1,DB,First,,3\nR2,OTHER,Second,,3\n";
  feed.dir_["trips.txt"] =
      "route_id,service_id,trip_id,trip_headsign,trip_short_name,block_id,"
      "bikes_allowed,wheelchair_accessible\n"
      "R1,WD,T1,To A,11,block,1,1\nR2,WD,T2,To B,22,block,2,2\n";
  for (auto const hour : {23, 26}) {
    SCOPED_TRACE(hour);
    feed.dir_["stop_times.txt"] = std::format(
        "trip_id,arrival_time,departure_time,stop_id,stop_sequence\n"
        "T1,23:00:00,23:00:00,P1,1\nT1,23:10:00,23:10:00,P2,2\n"
        "T1,{0:02}:30:00,{0:02}:30:00,A,3\n"
        "T2,{0:02}:35:00,{0:02}:35:00,A,1\n"
        "T2,{1:02}:10:00,{1:02}:10:00,B,2\n"
        "T2,{2:02}:00:00,{2:02}:00:00,P1,3\n",
        hour, hour + 2, hour + 3);
    auto const tt = load(feed);
    ASSERT_EQ(1U, tt.transport_traffic_days_.size());
    ASSERT_EQ(4U, tt.transport_to_trip_section_[transport_idx_t{0}].size());
    auto const expected = trip_events(tt);
    ASSERT_EQ(23U, expected.size());
    auto const path = root_ / std::to_string(hour) / output().filename();
    export_gtfs(tt, path);
    auto const exported = loader::make_dir(path);
    EXPECT_EQ(
        "attribution_id,route_id,organization_name,is_producer,attribution_"
        "url\n"
        "nigiri:0:0:0,0,::memory::/stop_times.txt,1,\n"
        "nigiri:0:1:0,1,::memory::/stop_times.txt,1,\n",
        exported->get_file("attributions.txt").data());
    auto const tt2 = load(*exported);
    EXPECT_EQ(2U, tt2.trip_ids_.size());
    EXPECT_EQ(expected, trip_events(tt2));

    auto shapes = shapes_storage{root_ / std::format("shapes-{}", hour),
                                 cista::mmap::protection::WRITE};
    for (auto trip = trip_idx_t{0}; trip != tt.n_trips(); ++trip) {
      auto const [transport, range] = tt.trip_transport_ranges_[trip].front();
      auto const stops = tt.route_location_seq_[tt.transport_route_[transport]];
      auto points = std::vector<geo::latlng>{};
      auto offsets = std::vector<shape_offset_t>{};
      for (auto s = range.from_; s != range.to_; ++s) {
        offsets.emplace_back(points.size());
        points.push_back(
            tt.locations_.coordinates_[stop{stops[s]}.location_idx()]);
      }
      auto const index = shape_idx_t{shapes.routed_data_.size()};
      shapes.routed_data_.emplace_back(points);
      shapes.add_trip_shape_offsets(
          trip, {to_scoped_shape_idx(index, shape_source::kRouted),
                 shapes.add_offsets(offsets)});
      auto const slice =
          gtfs_export::resolve_shape(tt, shapes, transport, trip, range);
      ASSERT_EQ(gtfs_export::shape_status::kValid, slice.status_);
      EXPECT_EQ(points, std::vector(begin(slice.points_), end(slice.points_)));
      EXPECT_EQ(0.0, slice.stop_distances_.front());
    }
    auto const shaped_path =
        root_ / std::format("shaped-{}", hour) / output().filename();
    export_gtfs(tt, shaped_path, &shapes);
    auto const credit_dir = loader::make_dir(shaped_path);
    auto const credits = credit_dir->get_file("attributions.txt");
    EXPECT_NE(std::string_view::npos,
              credits.data().find("osm,,OpenStreetMap contributors,1,"
                                  "https://www.openstreetmap.org/copyright\n"));
    auto imported_shapes =
        shapes_storage{root_ / std::format("imported-shapes-{}", hour),
                       cista::mmap::protection::WRITE};
    auto const imported =
        load(*loader::make_dir(shaped_path), kDateRange, &imported_shapes);
    EXPECT_EQ(expected, trip_events(imported));
    EXPECT_EQ(2U, imported_shapes.data_.size());
    for (auto trip = trip_idx_t{0}; trip != imported.n_trips(); ++trip) {
      EXPECT_EQ(3U, imported_shapes.get_shape(trip).size());
    }
  }
}

TEST_P(export_gtfs_test, expanded_frequencies) {
  auto feed = loader::mem_dir::read(kFeed);
  feed.dir_["frequencies.txt"] =
      "trip_id,start_time,end_time,headway_secs,exact_times\n"
      "T1,09:00:00,11:00:00,1800,1\n";
  auto const tt = load(feed);
  auto const expected = trip_events(tt);
  // Four departures on each of 23 service days, plus 31 trips for T2.
  ASSERT_EQ(4U * 23U + 31U, expected.size());
  export_gtfs(tt, output());
  auto const exported = loader::make_dir(output());
  EXPECT_FALSE(exported->exists("frequencies.txt"));
  EXPECT_EQ(expected, trip_events(load(*exported)));
}

TEST_P(export_gtfs_test, netex_and_gtfs) {
  auto const netex =
      loader::fs_dir{std::filesystem::path{NIGIRI_TEST_EXECUTION_DIR} /
                     "test/test_data/netex/export"};
  auto tt = timetable{};
  tt.date_range_ = kDateRange;
  tt.n_sources_ = 2;
  loader::register_special_stations(tt);
  auto bitfields = hash_map<bitfield, bitfield_idx_t>{};
  loader::netex::load_timetable({}, source_idx_t{0}, netex, tt, bitfields);
  auto expected = trip_events(tt);
  ASSERT_EQ(2U, expected.size());
  auto const gtfs = loader::mem_dir::read(kFeed);
  auto const gtfs_events = trip_events(load(gtfs));
  expected.insert(begin(gtfs_events), end(gtfs_events));
  loader::gtfs::load_timetable({}, source_idx_t{1}, gtfs, tt, bitfields);
  loader::finalize(tt);
  ASSERT_EQ(expected, trip_events(tt));

  export_gtfs(tt, output());
  auto const tt2 = load(*loader::make_dir(output()));
  EXPECT_EQ(expected, trip_events(tt2));
  EXPECT_EQ(stops(tt), stops(tt2));
}

TEST_P(export_gtfs_test, write_failure) {
  auto target = make_gtfs_export_target(output());
  auto& stream = target->create_file("test.txt");
  EXPECT_THROW(stream.setstate(std::ios::badbit), std::ios_base::failure);
  EXPECT_THROW(target->finalize(), std::ios_base::failure);
  target.reset();
  EXPECT_FALSE(std::filesystem::exists(output().string() + ".tmp_export"));
  if (is_zip()) {
    EXPECT_FALSE(std::filesystem::exists(output()));
  }
}

TEST_P(export_gtfs_test, preserve_existing_files) {
  auto const reserved =
      is_zip() ? std::filesystem::path{output().string() + ".tmp_export"}
               : output();
  std::filesystem::create_directory(reserved);
  auto const sentinel = reserved / "shapes.txt";
  {
    std::ofstream{sentinel} << "keep me";
  }
  EXPECT_ANY_THROW(make_gtfs_export_target(output()));
  EXPECT_EQ("keep me", loader::fs_dir{reserved}.get_file("shapes.txt").data());
}

TEST_P(export_gtfs_test, create_file_failure) {
  auto target = make_gtfs_export_target(output());
  EXPECT_ANY_THROW(target->create_file("missing/file.txt"));
}

TEST_P(export_gtfs_test, directory_zip_equivalence) {
  auto const tt = load(loader::mem_dir::read(kFeed));
  auto const directory = root_ / "directory";
  auto const zip = root_ / "archive.zip";
  auto shapes =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  add_shared_shape(tt, shapes, {{49.0, 8.0}, {50.0, 8.0}, {51.0, 8.0}});
  auto const storage = is_zip() ? &shapes : nullptr;
  export_gtfs(tt, directory, storage);
  export_gtfs(tt, zip, storage);
  auto const files = loader::fs_dir{directory};
  auto const archive = loader::zip_dir{zip};
  EXPECT_EQ(files.list_files({}).size(), archive.list_files({}).size());
  for (auto const& file : files.list_files({})) {
    EXPECT_EQ(files.get_file(file).data(), archive.get_file(file).data())
        << file;
  }
}

TEST_P(export_gtfs_test, incomplete_zip_preserves_previous_export) {
  if (!is_zip()) {
    GTEST_SKIP() << "ZIP publication test";
  }
  auto const tt = load(loader::mem_dir::read(kFeed));
  export_gtfs(tt, output());
  {
    auto target = make_gtfs_export_target(output());
    target->create_file("incomplete.txt") << "not finalized";
    EXPECT_EQ(trip_events(tt), trip_events(load(loader::zip_dir{output()})));
  }
  EXPECT_EQ(trip_events(tt), trip_events(load(loader::zip_dir{output()})));
  EXPECT_FALSE(std::filesystem::exists(output().string() + ".tmp_export"));
  export_gtfs(tt, output());
  EXPECT_EQ(trip_events(tt), trip_events(load(loader::zip_dir{output()})));
}

TEST_P(export_gtfs_test, parallel_bytes_match_serial) {
  auto feed = loader::mem_dir::read(kFeed);
  feed.dir_["frequencies.txt"] =
      "trip_id,start_time,end_time,headway_secs,exact_times\n"
      "T1,09:00:00,21:00:00,600,1\n";
  auto const tt = load(feed);
  ASSERT_GT(tt.transport_traffic_days_.size(), 32U);
  auto shapes =
      shapes_storage{root_ / "shapes", cista::mmap::protection::WRITE};
  add_shared_shape(tt, shapes, {{49.0, 8.0}, {50.0, 8.0}, {51.0, 8.0}});
  for (auto const shaped : {false, true}) {
    auto const storage = shaped ? &shapes : nullptr;
    auto const reference = root_ / (shaped ? "serial-shaped" : "serial");
    nigiri::export_gtfs(tt, reference, storage);
    auto const expected = loader::fs_dir{reference};
    for (auto const chunk : {1U, 7U, 64U}) {
      auto const path = root_ / std::format("parallel-{}-{}{}", shaped, chunk,
                                            is_zip() ? ".zip" : "");
      nigiri::export_gtfs(tt, path, storage, {4, chunk});
      auto const actual = loader::make_dir(path);
      ASSERT_EQ(expected.list_files({}).size(), actual->list_files({}).size());
      for (auto const& file : expected.list_files({})) {
        EXPECT_EQ(expected.get_file(file).data(), actual->get_file(file).data())
            << file << " chunk=" << chunk;
      }
    }
  }
}

TEST_P(export_gtfs_test, invalid_parallel_options_create_no_output) {
  auto const tt = load(loader::mem_dir::read(kFeed));
  for (auto const options :
       {gtfs_export_options{0, 1}, {257, 1}, {4, 0}, {4, 65537}}) {
    EXPECT_ANY_THROW(nigiri::export_gtfs(tt, output(), nullptr, options));
    EXPECT_FALSE(std::filesystem::exists(output()));
    EXPECT_FALSE(std::filesystem::exists(output().string() + ".tmp_export"));
  }
}

INSTANTIATE_TEST_SUITE_P(directory_and_zip,
                         export_gtfs_test,
                         testing::Combine(testing::Bool(),
                                          testing::Values(1U, 4U)));
