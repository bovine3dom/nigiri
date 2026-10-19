#pragma once

#include <condition_variable>
#include <cstddef>
#include <algorithm>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace nigiri::gtfs_export {

// Coordinators never run on this pool: waiting for a chunk cannot starve it.
class worker_pool {
public:
  explicit worker_pool(unsigned const threads) : window_{threads} {
    for (auto i = 0U; threads > 1U && i != threads; ++i) {
      workers_.emplace_back([this](std::stop_token const stop) {
        while (true) {
          auto job = std::function<void()>{};
          {
            auto lock = std::unique_lock{mutex_};
            if (!ready_.wait(lock, stop, [&] { return !jobs_.empty(); })) {
              return;
            }
            job = std::move(jobs_.front());
            jobs_.pop_front();
          }
          job();
        }
      });
    }
  }

  ~worker_pool() {
    for (auto& worker : workers_) {
      worker.request_stop();
    }
    workers_.clear();
  }

  void fail(std::exception_ptr const error) {
    auto lock = std::lock_guard{mutex_};
    if (!error_) {
      error_ = error;
    }
  }

  void check() {
    auto lock = std::lock_guard{mutex_};
    if (error_) {
      std::rethrow_exception(error_);
    }
  }

  template <typename Fn>
  auto submit(Fn fn) {
    using result = std::invoke_result_t<Fn>;
    auto task = std::make_shared<std::packaged_task<result()>>(
        [this, fn = std::move(fn)]() mutable {
          try {
            check();
            return fn();
          } catch (...) {
            fail(std::current_exception());
            throw;
          }
        });
    auto future = task->get_future();
    if (workers_.empty()) {
      (*task)();
    } else {
      {
        auto lock = std::lock_guard{mutex_};
        jobs_.emplace_back([task] { (*task)(); });
      }
      ready_.notify_one();
    }
    return future;
  }

  unsigned window() const { return window_; }

private:
  unsigned window_;
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<std::function<void()>> jobs_;
  std::exception_ptr error_;
  std::vector<std::jthread> workers_;
};

// Retain at most one window of chunks per coordinator, even if the first
// chunk is slow. Drain on failure before callbacks or captured state expire.
template <typename Produce, typename Consume>
void ordered_chunks(worker_pool& pool,
                    std::size_t const size,
                    std::size_t const chunk_size,
                    Produce produce,
                    Consume consume) {
  using result = std::invoke_result_t<Produce, std::size_t, std::size_t>;
  auto pending = std::deque<std::future<result>>{};
  auto next = std::size_t{0};
  try {
    while (next != size || !pending.empty()) {
      while (next != size && pending.size() < pool.window()) {
        pool.check();
        auto const from = next;
        auto const to = from + std::min(chunk_size, size - from);
        pending.emplace_back();
        pending.back() =
            pool.submit([&, from, to] { return produce(from, to); });
        next = to;
      }
      pool.check();
      consume(pending.front().get());
      pending.pop_front();
    }
  } catch (...) {
    pool.fail(std::current_exception());
    for (auto& future : pending) {
      if (future.valid()) {
        future.wait();
      }
    }
    throw;
  }
}

}  // namespace nigiri::gtfs_export
