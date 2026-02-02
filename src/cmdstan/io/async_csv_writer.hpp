#ifndef CMDSTAN_IO_ASYNC_CSV_WRITER_HPP
#define CMDSTAN_IO_ASYNC_CSV_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstdint>
#include <thread>
#include <atomic>
#include <array>
#include <condition_variable>
#include <mutex>
#include <memory>
#include <iomanip>

#ifdef STANBIN_USE_ZSTD
#include <zstd.h>
#endif

namespace cmdstan {
namespace io {

enum class CsvCompression : uint8_t {
  NONE = 0,
  ZSTD = 1
};

class async_csv_writer : public stan::callbacks::writer {
private:
  class Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit async_csv_writer(
      const std::string& path,
      CsvCompression compression = CsvCompression::NONE,
      int sig_figs = 6);
  
  ~async_csv_writer() override;
  
  async_csv_writer(async_csv_writer&& other) noexcept;
  async_csv_writer& operator=(async_csv_writer&& other) noexcept;
  
  async_csv_writer(const async_csv_writer&) = delete;
  async_csv_writer& operator=(const async_csv_writer&) = delete;
  
  void operator()(const std::vector<std::string>& names) override;
  void operator()(const std::vector<double>& state) override;
  void operator()(const std::string& message) override;
  void operator()() override;
  
  void finalize();
};

// ============================================================================
// Implementation
// ============================================================================

template<typename T, size_t Capacity>
class csv_spsc_ring_buffer {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
  
private:
  std::array<T, Capacity> buffer_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  
public:
  bool try_push(T&& item) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next_head = (head + 1) & (Capacity - 1);
    if (next_head == tail_.load(std::memory_order_acquire)) return false;
    buffer_[head] = std::move(item);
    head_.store(next_head, std::memory_order_release);
    return true;
  }
  
  bool try_pop(T& item) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    item = std::move(buffer_[tail]);
    tail_.store((tail + 1) & (Capacity - 1), std::memory_order_release);
    return true;
  }
  
  bool empty() const {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }
};

class async_csv_writer::Impl {
public:
  static constexpr size_t RING_BUFFER_SIZE = 256;
  
  std::string file_path_;
  CsvCompression compression_;
  int sig_figs_;
  
  std::ofstream stream_;
  std::atomic<size_t> num_rows_{0};
  std::atomic<bool> finalized_{false};
  
  // Message types for the queue
  struct Message {
    enum class Type { NAMES, VALUES, STRING, NEWLINE, SHUTDOWN };
    Type type;
    std::string data;  // Pre-formatted string
  };
  
  csv_spsc_ring_buffer<Message, RING_BUFFER_SIZE> ring_buffer_;
  std::thread writer_thread_;
  std::atomic<bool> shutdown_{false};
  std::mutex cv_mutex_;
  std::condition_variable cv_;
  
  // Compression state
#ifdef STANBIN_USE_ZSTD
  ZSTD_CStream* cstream_ = nullptr;
  std::vector<char> compress_out_buffer_;
#endif

  Impl(const std::string& path, CsvCompression compression, int sig_figs)
      : file_path_(compression == CsvCompression::ZSTD ? path + ".zst" : path),
        compression_(compression),
        sig_figs_(sig_figs),
        stream_(file_path_, std::ios::binary | std::ios::trunc) {
    
    if (!stream_) {
      throw std::runtime_error("Cannot open CSV output file: " + file_path_);
    }
    
#ifndef STANBIN_USE_ZSTD
    if (compression_ == CsvCompression::ZSTD) {
      throw std::runtime_error("ZSTD compression not available");
    }
#endif

#ifdef STANBIN_USE_ZSTD
    if (compression_ == CsvCompression::ZSTD) {
      cstream_ = ZSTD_createCStream();
      ZSTD_initCStream(cstream_, 1);  // Level 1 for speed
      compress_out_buffer_.resize(ZSTD_CStreamOutSize());
    }
#endif
    
    writer_thread_ = std::thread(&Impl::writer_loop, this);
  }

  ~Impl() {
    if (!finalized_.load()) {
      try { do_finalize(); } catch (...) {}
    }
#ifdef STANBIN_USE_ZSTD
    if (cstream_) {
      ZSTD_freeCStream(cstream_);
    }
#endif
  }

  void send_message(Message::Type type, std::string data = "") {
    Message msg{type, std::move(data)};
    while (!ring_buffer_.try_push(std::move(msg))) {
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, std::chrono::microseconds(100));
      if (shutdown_.load(std::memory_order_acquire)) {
        throw std::runtime_error("Writer thread terminated unexpectedly");
      }
      msg = Message{type, data};
    }
    cv_.notify_one();
  }

  void write_names(const std::vector<std::string>& names) {
    std::ostringstream ss;
    for (size_t i = 0; i < names.size(); ++i) {
      if (i > 0) ss << ',';
      ss << names[i];
    }
    ss << '\n';
    send_message(Message::Type::NAMES, ss.str());
  }

  void write_values(const std::vector<double>& values) {
    std::ostringstream ss;
    ss << std::setprecision(sig_figs_);
    for (size_t i = 0; i < values.size(); ++i) {
      if (i > 0) ss << ',';
      ss << values[i];
    }
    ss << '\n';
    send_message(Message::Type::VALUES, ss.str());
    ++num_rows_;
  }

  void write_string(const std::string& s) {
    send_message(Message::Type::STRING, "# " + s + "\n");
  }

  void write_newline() {
    send_message(Message::Type::NEWLINE, "\n");
  }

  void do_finalize() {
    if (finalized_.exchange(true)) return;
    
    shutdown_.store(true, std::memory_order_release);
    cv_.notify_all();
    
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    
#ifdef STANBIN_USE_ZSTD
    if (compression_ == CsvCompression::ZSTD && cstream_) {
      // Flush compression stream
      ZSTD_inBuffer in = {nullptr, 0, 0};
      ZSTD_outBuffer out = {compress_out_buffer_.data(), compress_out_buffer_.size(), 0};
      size_t remaining;
      do {
        remaining = ZSTD_endStream(cstream_, &out);
        stream_.write(compress_out_buffer_.data(), out.pos);
        out.pos = 0;
      } while (remaining > 0);
    }
#endif
    
    stream_.close();
    
    std::cerr << "CSV output: " << num_rows_.load() << " samples written to " 
              << file_path_ << std::endl;
  }

private:
  void writer_loop() {
    Message msg;
    while (true) {
      while (ring_buffer_.try_pop(msg)) {
        process_message(msg);
        cv_.notify_one();
      }
      if (shutdown_.load(std::memory_order_acquire)) {
        while (ring_buffer_.try_pop(msg)) {
          process_message(msg);
        }
        break;
      }
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
        return !ring_buffer_.empty() || shutdown_.load(std::memory_order_acquire);
      });
    }
  }

  void process_message(const Message& msg) {
    if (msg.type == Message::Type::SHUTDOWN) return;
    write_data(msg.data.data(), msg.data.size());
  }

  void write_data(const char* data, size_t size) {
    if (compression_ == CsvCompression::NONE) {
      stream_.write(data, size);
    } else {
#ifdef STANBIN_USE_ZSTD
      ZSTD_inBuffer in = {data, size, 0};
      while (in.pos < in.size) {
        ZSTD_outBuffer out = {compress_out_buffer_.data(), compress_out_buffer_.size(), 0};
        ZSTD_compressStream(cstream_, &out, &in);
        stream_.write(compress_out_buffer_.data(), out.pos);
      }
#endif
    }
  }
};

// Inline implementations
inline async_csv_writer::async_csv_writer(
    const std::string& path, CsvCompression compression, int sig_figs)
    : impl_(std::make_unique<Impl>(path, compression, sig_figs)) {}

inline async_csv_writer::~async_csv_writer() = default;

inline async_csv_writer::async_csv_writer(async_csv_writer&& other) noexcept = default;
inline async_csv_writer& async_csv_writer::operator=(async_csv_writer&& other) noexcept = default;

inline void async_csv_writer::operator()(const std::vector<std::string>& names) {
  impl_->write_names(names);
}

inline void async_csv_writer::operator()(const std::vector<double>& state) {
  impl_->write_values(state);
}

inline void async_csv_writer::operator()(const std::string& message) {
  impl_->write_string(message);
}

inline void async_csv_writer::operator()() {
  impl_->write_newline();
}

inline void async_csv_writer::finalize() {
  if (impl_) impl_->do_finalize();
}

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_ASYNC_CSV_WRITER_HPP
