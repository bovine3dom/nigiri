#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

namespace nigiri {

struct timetable;
struct shapes_storage;

struct gtfs_export_target {
  virtual ~gtfs_export_target() = default;

  virtual std::ostream& create_file(std::string const& filename) = 0;

  virtual void finalize() {}
};

struct gtfs_dir_export_target : public gtfs_export_target {
  explicit gtfs_dir_export_target(std::filesystem::path dir);

  std::ostream& create_file(std::string const& filename) override;
  void finalize() override;

  std::filesystem::path dir_;
  std::vector<std::unique_ptr<std::ofstream>> streams_;
};

struct gtfs_zip_export_target : public gtfs_export_target {
  explicit gtfs_zip_export_target(std::filesystem::path zip_path);
  ~gtfs_zip_export_target() override;

  gtfs_zip_export_target(gtfs_zip_export_target const&) = delete;
  gtfs_zip_export_target(gtfs_zip_export_target&&) = delete;
  gtfs_zip_export_target& operator=(gtfs_zip_export_target const&) = delete;
  gtfs_zip_export_target& operator=(gtfs_zip_export_target&&) = delete;

  std::ostream& create_file(std::string const& filename) override;
  void finalize() override;

  std::filesystem::path zip_path_;
  std::filesystem::path tmp_dir_;
  std::vector<std::string> filenames_;
  std::vector<std::unique_ptr<std::ofstream>> streams_;

  struct impl;  // hides mz_zip_archive (miniz) from this header
  std::unique_ptr<impl> impl_;
};

std::unique_ptr<gtfs_export_target> make_gtfs_export_target(
    std::filesystem::path const& output_path);

void export_gtfs(timetable const& tt,
                 std::filesystem::path const& output_path,
                 shapes_storage const* shapes = nullptr);

}  // namespace nigiri
