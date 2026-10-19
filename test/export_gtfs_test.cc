#include "gtest/gtest.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "nigiri/loader/dir.h"
#include "nigiri/loader/gtfs/load_timetable.h"
#include "nigiri/loader/init_finish.h"
#include "nigiri/loader/netex/load_timetable.h"
#include "nigiri/export_gtfs.h"
#include "nigiri/rt/frun.h"
#include "nigiri/special_stations.h"
#include "nigiri/timetable.h"

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
               interval<sys_days> const dates = kDateRange) {
  auto tt = timetable{};
  tt.date_range_ = dates;
  loader::register_special_stations(tt);
  loader::gtfs::load_timetable({}, source_idx_t{0}, d, tt);
  loader::finalize(tt);
  return tt;
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

struct export_gtfs_test : testing::TestWithParam<bool> {
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
    return root_ / (GetParam() ? "feed.ZIP" : "feed");
  }

  std::filesystem::path root_;
};

}  // namespace

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

  export_gtfs(tt, output());

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

TEST_P(export_gtfs_test, colliding_source_ids) {
  auto const first = loader::mem_dir::read(kFeed);
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
    loader::finalize(tt);
    EXPECT_EQ(expected, trip_events(tt));

    auto const path =
        root_ / (identical ? "identical" : "different") / output().filename();
    export_gtfs(tt, path);
    auto const tt2 = load(*loader::make_dir(path));
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
    auto const tt2 = load(*loader::make_dir(path));
    EXPECT_EQ(2U, tt2.trip_ids_.size());
    EXPECT_EQ(expected, trip_events(tt2));
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
  if (GetParam()) {
    EXPECT_FALSE(std::filesystem::exists(output()));
  }
}

TEST_P(export_gtfs_test, preserve_existing_files) {
  auto const reserved =
      GetParam() ? std::filesystem::path{output().string() + ".tmp_export"}
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
  export_gtfs(tt, directory);
  export_gtfs(tt, zip);
  auto const files = loader::fs_dir{directory};
  auto const archive = loader::zip_dir{zip};
  EXPECT_EQ(files.list_files({}).size(), archive.list_files({}).size());
  for (auto const& file : files.list_files({})) {
    EXPECT_EQ(files.get_file(file).data(), archive.get_file(file).data())
        << file;
  }
}

TEST_P(export_gtfs_test, incomplete_zip_preserves_previous_export) {
  if (!GetParam()) {
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

INSTANTIATE_TEST_SUITE_P(directory_and_zip, export_gtfs_test, testing::Bool());
