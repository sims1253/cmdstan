#ifndef CMDSTAN_IO_STANBIN_WRITER_HPP
#define CMDSTAN_IO_STANBIN_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstring>
#include <cstdint>

namespace cmdstan {
namespace io {

/**
 * Single-file binary writer for Stan MCMC output.
 *
 * Writes a self-contained .stanbin file with embedded header containing
 * dimensions and column names, followed by raw 64-bit doubles.
 */
class stanbin_writer : public stan::callbacks::writer {
 private:
  static constexpr char MAGIC[8] = {'S', 'T', 'A', 'N', 'B', 'I', 'N', '\0'};
  static constexpr uint32_t VERSION = 1;
  static constexpr std::size_t HEADER_SIZE = 64;

  std::string file_path_;
  std::ofstream stream_;
  std::vector<std::string> names_;
  std::size_t num_rows_ = 0;
  std::size_t num_cols_ = 0;
  bool header_written_ = false;
  bool finalized_ = false;
  std::size_t data_start_offset_ = 0;

 public:
  explicit stanbin_writer(const std::string& path)
      : file_path_(path.ends_with(".stanbin") ? path : path + ".stanbin"),
        stream_(file_path_, std::ios::binary | std::ios::trunc) {
    if (!stream_) {
      throw std::runtime_error("Cannot open stanbin output file: " + file_path_);
    }
    // Write placeholder header (will be updated at finalize)
    write_placeholder_header();
  }

  ~stanbin_writer() override {
    try {
      if (!finalized_ && stream_.is_open()) {
        finalize();
      }
    } catch (...) {}
  }

  stanbin_writer(const stanbin_writer&) = delete;
  stanbin_writer& operator=(const stanbin_writer&) = delete;

  stanbin_writer(stanbin_writer&& other) noexcept
      : file_path_(std::move(other.file_path_)),
        stream_(std::move(other.stream_)),
        names_(std::move(other.names_)),
        num_rows_(other.num_rows_),
        num_cols_(other.num_cols_),
        header_written_(other.header_written_),
        finalized_(other.finalized_),
        data_start_offset_(other.data_start_offset_) {
    other.finalized_ = true;
  }

  stanbin_writer& operator=(stanbin_writer&& other) noexcept {
    if (this != &other) {
      if (!finalized_ && stream_.is_open()) {
        finalize();
      }
      file_path_ = std::move(other.file_path_);
      stream_ = std::move(other.stream_);
      names_ = std::move(other.names_);
      num_rows_ = other.num_rows_;
      num_cols_ = other.num_cols_;
      header_written_ = other.header_written_;
      finalized_ = other.finalized_;
      data_start_offset_ = other.data_start_offset_;
      other.finalized_ = true;
    }
    return *this;
  }

  void operator()(const std::vector<std::string>& names) override {
    if (header_written_) {
      if (names.size() != num_cols_) {
        throw std::runtime_error("stanbin_writer: column count mismatch");
      }
      return;
    }
    names_ = names;
    num_cols_ = names.size();
    
    // Write names section
    write_names_section();
    data_start_offset_ = stream_.tellp();
    header_written_ = true;
  }

  void operator()(const std::vector<double>& state) override {
    if (state.empty()) return;
    
    stream_.write(reinterpret_cast<const char*>(state.data()),
                  static_cast<std::streamsize>(state.size() * sizeof(double)));
    if (!stream_) {
      throw std::runtime_error("stanbin_writer: failed to write sample data");
    }
    ++num_rows_;
  }

  void operator()(const std::string&) override {}
  void operator()() override {}

  void finalize() {
    if (finalized_) return;

    stream_.close();

    // Update header with final row count
    update_header();

    finalized_ = true;
    std::cerr << "Stanbin output: " << num_rows_ << " samples, "
              << num_cols_ << " parameters written to " << file_path_ << std::endl;
  }

  std::size_t num_rows() const { return num_rows_; }
  std::size_t num_cols() const { return num_cols_; }

 private:
  void write_placeholder_header() {
    std::vector<char> header(HEADER_SIZE, 0);
    
    // Magic
    std::memcpy(header.data(), MAGIC, 8);
    
    // Version
    uint32_t version = VERSION;
    std::memcpy(header.data() + 8, &version, 4);
    
    // Flags (will be updated at finalize)
    uint32_t flags = 0;
    std::memcpy(header.data() + 12, &flags, 4);
    
    // Rows, Cols, Header size, Names size - all zeros for now
    
    stream_.write(header.data(), HEADER_SIZE);
  }

  void write_names_section() {
    for (const auto& name : names_) {
      stream_.write(name.c_str(), name.size() + 1);  // Include null terminator
    }
  }

  std::size_t calculate_names_size() const {
    std::size_t size = 0;
    for (const auto& name : names_) {
      size += name.size() + 1;  // Include null terminator
    }
    return size;
  }

  void update_header() {
    std::fstream file(file_path_, std::ios::binary | std::ios::in | std::ios::out);
    if (!file) {
      throw std::runtime_error("Cannot reopen file to update header: " + file_path_);
    }
    
    // Seek to flags position and write (always 0, no compression)
    file.seekp(12);
    uint32_t flags = 0;
    file.write(reinterpret_cast<const char*>(&flags), 4);
    
    // Rows
    uint64_t rows = num_rows_;
    file.write(reinterpret_cast<const char*>(&rows), 8);
    
    // Cols
    uint64_t cols = num_cols_;
    file.write(reinterpret_cast<const char*>(&cols), 8);
    
    // Header size (offset to data)
    uint32_t names_size = static_cast<uint32_t>(calculate_names_size());
    uint32_t header_size = static_cast<uint32_t>(HEADER_SIZE + names_size);
    file.write(reinterpret_cast<const char*>(&header_size), 4);
    
    // Names size
    file.write(reinterpret_cast<const char*>(&names_size), 4);
  }
};

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_STANBIN_WRITER_HPP
