#include <filesystem>
#include <iostream>
#include <memory>

#include "boost/program_options.hpp"

#include "nigiri/export_gtfs.h"
#include "nigiri/shapes_storage.h"
#include "nigiri/timetable.h"
#include "utl/verify.h"

using namespace nigiri;

int main(int argc, char* argv[]) try {
  namespace bpo = boost::program_options;

  auto tt_path = std::filesystem::path{};
  auto gtfs_dir = std::filesystem::path{};
  auto shapes_dir = std::filesystem::path{};
  auto options = gtfs_export_options{};

  auto desc = bpo::options_description{"Allowed options"};
  desc.add_options()("help,h", "produce this help message")  //
      ("tt_path,p", bpo::value(&tt_path)->required(),
       "path to a binary file containing a serialized nigiri timetable")  //
      ("threads", bpo::value(&options.threads_)->default_value(1),
       "chunk worker count (1-256); 1 writes tables sequentially")  //
      ("chunk-size", bpo::value(&options.chunk_size_)->default_value(64),
       "transports per chunk (1-65536)")  //
      ("shapes", bpo::value(&shapes_dir),
       "shape directory from the same import as the timetable (read-only)")  //
      ("gtfs_export_dir,o", bpo::value(&gtfs_dir)->required(),
       "output path: either a directory (e.g. -o output/) to export the "
       "individual GTFS files into, or a path ending in \".zip\" (e.g. -o "
       "output.zip) to export a single zip archive");
  bpo::variables_map vm;
  bpo::store(bpo::command_line_parser(argc, argv).options(desc).run(), vm);

  if (vm.count("help") != 0U) {
    std::cout << desc << "\n";
    return 0;
  }

  bpo::notify(vm);
  utl::verify(options.threads_ >= 1U && options.threads_ <= 256U &&
                  options.chunk_size_ >= 1U && options.chunk_size_ <= 65536U,
              "Invalid export thread count or chunk size");

  std::cout << "loading timetable...\n";
  auto tt = nigiri::timetable::read(tt_path);
  tt->resolve();

  auto shapes = std::unique_ptr<shapes_storage>{};
  if (vm.count("shapes") != 0U) {
    // Check first: the storage constructor can create a missing directory.
    for (auto const file :
         {"shapes_data.bin", "shapes_idx.bin", "routed_shapes_data.bin",
          "routed_shapes_idx.bin", "shape_offsets_data.bin",
          "shape_offsets_idx.bin", "shape_trip_offsets.bin",
          "shape_route_bboxes.bin", "shape_route_segment_bboxes_data.bin",
          "shape_route_segment_bboxes_idx.bin"}) {
      utl::verify(std::filesystem::is_regular_file(shapes_dir / file),
                  "Missing shape storage file: {}",
                  (shapes_dir / file).string());
    }
    shapes = std::make_unique<shapes_storage>(shapes_dir,
                                              cista::mmap::protection::READ);
    utl::verify(shapes->trip_offset_indices_.size() == tt->n_trips(),
                "Shape storage trip count does not match the timetable");
  }

  std::cout << "exporting timetable as GTFS...\n";
  export_gtfs(*tt, gtfs_dir, shapes.get(), options);
  std::cout << "done.\n";

  return 0;
} catch (std::exception const& e) {
  std::cerr << "GTFS export failed: " << e.what() << "\n";
  return 1;
}
