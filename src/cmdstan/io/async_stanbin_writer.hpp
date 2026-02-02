#ifndef CMDSTAN_IO_ASYNC_STANBIN_WRITER_HPP
#define CMDSTAN_IO_ASYNC_STANBIN_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <array>
#include <condition_variable>
#include <mutex>
#include <memory>

#ifdef STANBIN_USE_LZ4
#include <lz4.h>
#endif

#ifdef STANBIN_USE_ZSTD
#include <zstd.h>
#endif

namespace cmdstan {
namespace io {

enum class StanbinCompression : uint8_t {
  NONE = 0,
  LZ4 = 1,
  ZSTD = 2
};

/**
 * Async binary writer for Stan MCMC output with optional compression.
 * Uses Pimpl idiom to be moveable despite containing thread/atomics.
 */
class async_stanbin_writer : public stan::callbacks::writer {
private:
  class Impl;
  std::unique_ptr<Impl> impl_;

public:
  explicit async_stanbin_writer(
      const std::string& path,
      StanbinCompression compression = StanbinCompression::NONE,
      size_t block_size = 64,
      bool enable_checksum = true);
  
  ~async_stanbin_writer() override;
  
  // Moveable
  async_stanbin_writer(async_stanbin_writer&& other) noexcept;
  async_stanbin_writer& operator=(async_stanbin_writer&& other) noexcept;
  
  // Non-copyable
  async_stanbin_writer(const async_stanbin_writer&) = delete;
  async_stanbin_writer& operator=(const async_stanbin_writer&) = delete;
  
  void operator()(const std::vector<std::string>& names) override;
  void operator()(const std::vector<double>& state) override;
  void operator()(const std::string&) override {}
  void operator()() override {}
  
  void finalize();
  size_t num_rows() const;
  size_t num_cols() const;
};

// ============================================================================
// Implementation
// ============================================================================

template<typename T, size_t Capacity>
class spsc_ring_buffer {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
  
private:
  std::array<T, Capacity> buffer_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  
public:
  bool try_push(const T& item) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t next_head = (head + 1) & (Capacity - 1);
    if (next_head == tail_.load(std::memory_order_acquire)) return false;
    buffer_[head] = item;
    head_.store(next_head, std::memory_order_release);
    return true;
  }
  
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

class crc32 {
private:
  uint32_t crc_ = 0xFFFFFFFF;
  static constexpr uint32_t POLYNOMIAL = 0xEDB88320;
  
  static const uint32_t* get_table() {
    static uint32_t table[256] = {0};
    static bool initialized = false;
    if (!initialized) {
      for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int j = 0; j < 8; ++j) {
          c = (c >> 1) ^ ((c & 1) ? POLYNOMIAL : 0);
        }
        table[i] = c;
      }
      initialized = true;
    }
    return table;
  }
  
public:
  void update(const void* data, size_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    const uint32_t* table = get_table();
    for (size_t i = 0; i < length; ++i) {
      crc_ = table[(crc_ ^ bytes[i]) & 0xFF] ^ (crc_ >> 8);
    }
  }
  
  uint32_t finalize() const { return crc_ ^ 0xFFFFFFFF; }
  void reset() { crc_ = 0xFFFFFFFF; }
};

class async_stanbin_writer::Impl {
public:
  static constexpr char MAGIC[8] = {'S', 'T', 'A', 'N', 'B', 'I', 'N', '\0'};
  static constexpr uint32_t VERSION = 2;
  static constexpr size_t HEADER_SIZE = 64;
  static constexpr size_t RING_BUFFER_SIZE = 128;
  static constexpr uint32_t TRAILER_MAGIC = 0x4E494254;
  static constexpr uint32_t FLAG_COMPRESSION_MASK = 0x03;
  static constexpr uint32_t FLAG_CHECKSUM = 0x04;
  static constexpr uint32_t FLAG_HAS_TRAILER = 0x08;
  
  std::string file_path_;
  StanbinCompression compression_;
  size_t block_size_;
  bool enable_checksum_;
  
  std::ofstream stream_;
  std::vector<std::string> names_;
  std::atomic<size_t> num_rows_{0};
  size_t num_cols_ = 0;
  bool header_written_ = false;
  std::atomic<bool> finalized_{false};
  size_t data_start_offset_ = 0;
  
  using SampleBuffer = std::vector<double>;
  spsc_ring_buffer<SampleBuffer, RING_BUFFER_SIZE> ring_buffer_;
  std::thread writer_thread_;
  std::atomic<bool> shutdown_{false};
  std::mutex cv_mutex_;
  std::condition_variable cv_;
  
  std::vector<double> block_buffer_;
  size_t rows_in_block_ = 0;
  size_t block_count_ = 0;
  std::vector<char> compress_buffer_;
  crc32 checksum_;

  Impl(const std::string& path, StanbinCompression compression, size_t block_size, bool enable_checksum)
      : file_path_(path.ends_with(".stanbin") ? path : path + ".stanbin"),
        compression_(compression),
        block_size_(block_size),
        enable_checksum_(enable_checksum),
        stream_(file_path_, std::ios::binary | std::ios::trunc) {
    
    if (!stream_) {
      throw std::runtime_error("Cannot open stanbin output file: " + file_path_);
    }
    
#ifndef STANBIN_USE_LZ4
    if (compression_ == StanbinCompression::LZ4) {
      throw std::runtime_error("LZ4 compression not available");
    }
#endif
#ifndef STANBIN_USE_ZSTD
    if (compression_ == StanbinCompression::ZSTD) {
      throw std::runtime_error("ZSTD compression not available");
    }
#endif
    
    write_placeholder_header();
    writer_thread_ = std::thread(&Impl::writer_loop, this);
  }

  ~Impl() {
    if (!finalized_.load()) {
      try { do_finalize(); } catch (...) {}
    }
  }

  void write_names(const std::vector<std::string>& names) {
    if (header_written_) {
      if (names.size() != num_cols_) {
        throw std::runtime_error("async_stanbin_writer: column count mismatch");
      }
      return;
    }
    names_ = names;
    num_cols_ = names.size();
    block_buffer_.reserve(block_size_ * num_cols_);
    
    if (compression_ != StanbinCompression::NONE) {
      size_t max_block_bytes = block_size_ * num_cols_ * sizeof(double);
#ifdef STANBIN_USE_LZ4
      if (compression_ == StanbinCompression::LZ4) {
        compress_buffer_.resize(LZ4_compressBound(max_block_bytes));
      }
#endif
#ifdef STANBIN_USE_ZSTD
      if (compression_ == StanbinCompression::ZSTD) {
        compress_buffer_.resize(ZSTD_compressBound(max_block_bytes));
      }
#endif
    }
    
    write_names_section();
    data_start_offset_ = stream_.tellp();
    header_written_ = true;
  }

  void write_sample(const std::vector<double>& state) {
    if (state.empty()) return;
    
    SampleBuffer copy = state;
    while (!ring_buffer_.try_push(std::move(copy))) {
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, std::chrono::microseconds(100));
      if (shutdown_.load(std::memory_order_acquire)) {
        throw std::runtime_error("Writer thread terminated unexpectedly");
      }
      copy = state;
    }
    cv_.notify_one();
  }

  void do_finalize() {
    if (finalized_.exchange(true)) return;
    
    shutdown_.store(true, std::memory_order_release);
    cv_.notify_all();
    
    if (writer_thread_.joinable()) {
      writer_thread_.join();
    }
    
    flush_block_final();
    write_trailer();
    stream_.close();
    update_header();
    
    std::cerr << "Stanbin output: " << num_rows_.load() << " samples, "
              << num_cols_ << " parameters";
    if (compression_ != StanbinCompression::NONE) {
      std::cerr << " (" << (compression_ == StanbinCompression::LZ4 ? "LZ4" : "ZSTD")
                << ", " << block_count_ << " blocks)";
    }
    std::cerr << " written to " << file_path_ << std::endl;
  }

private:
  void writer_loop() {
    SampleBuffer sample;
    while (true) {
      while (ring_buffer_.try_pop(sample)) {
        process_sample(sample);
        cv_.notify_one();
      }
      if (shutdown_.load(std::memory_order_acquire)) {
        while (ring_buffer_.try_pop(sample)) {
          process_sample(sample);
        }
        break;
      }
      std::unique_lock<std::mutex> lock(cv_mutex_);
      cv_.wait_for(lock, std::chrono::milliseconds(1), [this] {
        return !ring_buffer_.empty() || shutdown_.load(std::memory_order_acquire);
      });
    }
  }

  void process_sample(const SampleBuffer& sample) {
    if (enable_checksum_) {
      checksum_.update(sample.data(), sample.size() * sizeof(double));
    }
    
    // Always batch if block_size_ > 1, compress only if compression enabled
    if (block_size_ > 1) {
      block_buffer_.insert(block_buffer_.end(), sample.begin(), sample.end());
      ++rows_in_block_;
      if (rows_in_block_ >= block_size_) {
        flush_block();
      }
    } else {
      // No batching, write immediately
      write_raw(sample.data(), sample.size() * sizeof(double));
    }
    ++num_rows_;
  }

  void write_raw(const void* data, size_t size) {
    stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream_) {
      throw std::runtime_error("async_stanbin_writer: failed to write data");
    }
  }

  void flush_block() {
    if (block_buffer_.empty()) return;
    
    if (compression_ == StanbinCompression::NONE) {
      // Batched but uncompressed - just write the raw buffer
      write_raw(block_buffer_.data(), block_buffer_.size() * sizeof(double));
    } else {
      // Compress and write
      const char* src = reinterpret_cast<const char*>(block_buffer_.data());
      size_t src_size = block_buffer_.size() * sizeof(double);
      size_t compressed_size = 0;

#ifdef STANBIN_USE_LZ4
      if (compression_ == StanbinCompression::LZ4) {
        compressed_size = LZ4_compress_default(src, compress_buffer_.data(),
            static_cast<int>(src_size), static_cast<int>(compress_buffer_.size()));
        if (compressed_size == 0) throw std::runtime_error("LZ4 compression failed");
      }
#endif
#ifdef STANBIN_USE_ZSTD
      if (compression_ == StanbinCompression::ZSTD) {
        compressed_size = ZSTD_compress(compress_buffer_.data(), compress_buffer_.size(), src, src_size, 1);
        if (ZSTD_isError(compressed_size)) {
          throw std::runtime_error(std::string("ZSTD compression failed: ") + ZSTD_getErrorName(compressed_size));
        }
      }
#endif
      
      uint32_t compressed_u32 = static_cast<uint32_t>(compressed_size);
      uint32_t uncompressed_u32 = static_cast<uint32_t>(src_size);
      write_raw(&compressed_u32, sizeof(compressed_u32));
      write_raw(&uncompressed_u32, sizeof(uncompressed_u32));
      write_raw(compress_buffer_.data(), compressed_size);
    }
    
    ++block_count_;
    block_buffer_.clear();
    rows_in_block_ = 0;
  }

  void flush_block_final() {
    if (block_buffer_.empty()) return;
    if (compression_ == StanbinCompression::NONE) {
      write_raw(block_buffer_.data(), block_buffer_.size() * sizeof(double));
    } else {
      flush_block();
    }
    block_buffer_.clear();
    rows_in_block_ = 0;
  }

  void write_placeholder_header() {
    std::vector<char> header(HEADER_SIZE, 0);
    std::memcpy(header.data(), MAGIC, 8);
    uint32_t version = VERSION;
    std::memcpy(header.data() + 8, &version, 4);
    stream_.write(header.data(), HEADER_SIZE);
  }

  void write_names_section() {
    for (const auto& name : names_) {
      stream_.write(name.c_str(), name.size() + 1);
    }
  }

  size_t calculate_names_size() const {
    size_t size = 0;
    for (const auto& name : names_) size += name.size() + 1;
    return size;
  }

  void write_trailer() {
    if (enable_checksum_) {
      uint32_t crc = checksum_.finalize();
      write_raw(&crc, sizeof(crc));
    }
    if (compression_ != StanbinCompression::NONE) {
      uint32_t bc = static_cast<uint32_t>(block_count_);
      write_raw(&bc, sizeof(bc));
    }
    if (enable_checksum_ || compression_ != StanbinCompression::NONE) {
      uint32_t trailer_magic = TRAILER_MAGIC;
      write_raw(&trailer_magic, sizeof(trailer_magic));
    }
  }

  void update_header() {
    std::fstream file(file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) throw std::runtime_error("Cannot reopen file to update header: " + file_path_);
    
    uint32_t flags = static_cast<uint32_t>(compression_) & FLAG_COMPRESSION_MASK;
    if (enable_checksum_) flags |= FLAG_CHECKSUM;
    if (enable_checksum_ || compression_ != StanbinCompression::NONE) flags |= FLAG_HAS_TRAILER;
    
    file.seekp(12);
    file.write(reinterpret_cast<const char*>(&flags), 4);
    
    uint64_t rows = num_rows_.load();
    file.write(reinterpret_cast<const char*>(&rows), 8);
    
    uint64_t cols = num_cols_;
    file.write(reinterpret_cast<const char*>(&cols), 8);
    
    uint32_t names_size = static_cast<uint32_t>(calculate_names_size());
    uint32_t header_size = static_cast<uint32_t>(HEADER_SIZE + names_size);
    file.write(reinterpret_cast<const char*>(&header_size), 4);
    file.write(reinterpret_cast<const char*>(&names_size), 4);
    
    uint32_t block_size_u32 = static_cast<uint32_t>(block_size_);
    file.write(reinterpret_cast<const char*>(&block_size_u32), 4);
  }
};

// Inline implementations of async_stanbin_writer methods
inline async_stanbin_writer::async_stanbin_writer(
    const std::string& path, StanbinCompression compression, size_t block_size, bool enable_checksum)
    : impl_(std::make_unique<Impl>(path, compression, block_size, enable_checksum)) {}

inline async_stanbin_writer::~async_stanbin_writer() = default;

inline async_stanbin_writer::async_stanbin_writer(async_stanbin_writer&& other) noexcept = default;
inline async_stanbin_writer& async_stanbin_writer::operator=(async_stanbin_writer&& other) noexcept = default;

inline void async_stanbin_writer::operator()(const std::vector<std::string>& names) {
  impl_->write_names(names);
}

inline void async_stanbin_writer::operator()(const std::vector<double>& state) {
  impl_->write_sample(state);
}

inline void async_stanbin_writer::finalize() {
  if (impl_) impl_->do_finalize();
}

inline size_t async_stanbin_writer::num_rows() const {
  return impl_ ? impl_->num_rows_.load() : 0;
}

inline size_t async_stanbin_writer::num_cols() const {
  return impl_ ? impl_->num_cols_ : 0;
}

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_ASYNC_STANBIN_WRITER_HPP
