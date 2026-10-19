#include "gtest/gtest.h"

#include <chrono>
#include <atomic>
#include <future>
#include <numeric>
#include <stdexcept>
#include <vector>

#include "../src/gtfs_export/parallel.h"

using namespace nigiri::gtfs_export;
using namespace std::chrono_literals;

TEST(export_parallel, ordered_and_bounded) {
  auto pool = worker_pool{4};
  auto started = std::atomic<unsigned>{0};
  auto release = std::promise<void>{};
  auto gate = release.get_future().share();
  auto output = std::vector<std::size_t>{};
  auto run = std::async(std::launch::async, [&] {
    ordered_chunks(
        pool, 31, 1,
        [&](std::size_t from, std::size_t) {
          ++started;
          if (from == 0) {
            if (gate.wait_for(10s) != std::future_status::ready) {
              throw std::runtime_error{"gate timeout"};
            }
          }
          return from;
        },
        [&](std::size_t value) { output.push_back(value); });
  });
  auto const deadline = std::chrono::steady_clock::now() + 5s;
  while (started.load() < 4 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  // The blocked first chunk prevents an unbounded queue of completed chunks.
  EXPECT_EQ(4U, started.load());
  release.set_value();
  run.get();
  auto expected = std::vector<std::size_t>(31);
  std::iota(begin(expected), end(expected), 0U);
  EXPECT_EQ(expected, output);
}

TEST(export_parallel, shared_pool_runs_two_tables) {
  auto pool = worker_pool{2};
  auto started = std::atomic<unsigned>{0};
  auto release = std::promise<void>{};
  auto gate = release.get_future().share();
  auto table = [&] {
    auto rows = std::vector<std::size_t>{};
    ordered_chunks(
        pool, 7, 3,
        [&](std::size_t from, std::size_t to) {
          if (from == 0) {
            ++started;
            if (gate.wait_for(10s) != std::future_status::ready) {
              throw std::runtime_error{"gate timeout"};
            }
          }
          return to - from;
        },
        [&](std::size_t size) { rows.push_back(size); });
    return rows;
  };
  auto a = std::async(std::launch::async, table);
  auto b = std::async(std::launch::async, table);
  auto const deadline = std::chrono::steady_clock::now() + 5s;
  while (started.load() < 2 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(2U, started.load());
  release.set_value();
  EXPECT_EQ((std::vector<std::size_t>{3, 3, 1}), a.get());
  EXPECT_EQ((std::vector<std::size_t>{3, 3, 1}), b.get());
}

TEST(export_parallel, exceptions_cancel_and_drain) {
  for (auto const producer_failure : {false, true}) {
    auto pool = worker_pool{4};
    auto active = std::atomic<unsigned>{0};
    auto visited = std::atomic<unsigned>{0};
    EXPECT_THROW(
        ordered_chunks(
            pool, 1000, 1,
            [&](std::size_t from, std::size_t) {
              ++visited;
              if (from == 0 && producer_failure) {
                throw std::runtime_error{"producer failed"};
              }
              ++active;
              std::this_thread::sleep_for(2ms);
              --active;
              return from;
            },
            [&](std::size_t) { throw std::runtime_error{"writer failed"}; }),
        std::runtime_error);
    EXPECT_EQ(0U, active.load());
    EXPECT_LE(visited.load(), 4U);
    EXPECT_THROW(pool.check(), std::runtime_error);
  }
}

TEST(export_parallel, empty_range) {
  auto pool = worker_pool{2};
  ordered_chunks(
      pool, 0, 1,
      [](std::size_t, std::size_t) {
        ADD_FAILURE();
        return 0;
      },
      [](int) { ADD_FAILURE(); });
}
