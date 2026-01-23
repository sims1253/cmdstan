#ifndef CMDSTAN_IO_BIN_WRITER_HPP
#define CMDSTAN_IO_BIN_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>

namespace cmdstan {
namespace io {

/**
 * Binary writer for Stan MCMC output.
 *
 * Writes raw 64-bit doubles in row-major order to a .bin file,
 * with a companion .meta file containing dimensions and column names.
 *
 */
class bin_writer : public stan::callbacks::writer {
 private:
  std::ofstream data_stream_;
  std::string base_path_;
  std::vector<std::string> names_;
  std::size_t num_rows_ = 0;
  std::size_t num_cols_ = 0;
  bool header_written_ = false;
  bool finalized_ = false;

 public:
  /**
   * Construct a binary writer.
   *
   * @param base_path Path without extension. Will create {base_path}.bin
   *                  and {base_path}.meta files.
   */
  explicit bin_writer(const std::string& base_path)
      : base_path_(base_path),
        data_stream_(base_path + ".bin", std::ios::binary | std::ios::trunc) {
    if (!data_stream_) {
      throw std::runtime_error("Cannot open binary output file: " 
                               + base_path + ".bin");
    }
  }

  /**
   * Destructor ensures finalize() is called.
   */
  ~bin_writer() override {
    try {
      if (!finalized_ && data_stream_.is_open()) {
        finalize();
      }
    } catch (...) {
      // Destructors must not throw
    }
  }

  bin_writer(const bin_writer&) = delete;
  bin_writer& operator=(const bin_writer&) = delete;

  bin_writer(bin_writer&& other) noexcept
      : data_stream_(std::move(other.data_stream_)),
        base_path_(std::move(other.base_path_)),
        names_(std::move(other.names_)),
        num_rows_(other.num_rows_),
        num_cols_(other.num_cols_),
        header_written_(other.header_written_),
        finalized_(other.finalized_) {
    other.finalized_ = true;
  }

  bin_writer& operator=(bin_writer&& other) noexcept {
    if (this != &other) {
      if (!finalized_ && data_stream_.is_open()) {
        finalize();
      }
      data_stream_ = std::move(other.data_stream_);
      base_path_ = std::move(other.base_path_);
      names_ = std::move(other.names_);
      num_rows_ = other.num_rows_;
      num_cols_ = other.num_cols_;
      header_written_ = other.header_written_;
      finalized_ = other.finalized_;
      other.finalized_ = true;
    }
    return *this;
  }

  /**
   * Record parameter names. Called once at the start of sampling.
   * 
   * @param names Vector of parameter names
   */
  void operator()(const std::vector<std::string>& names) override {
    if (header_written_) {
      // Guard against multiple calls (can happen in some multi-chain configs)
      if (names.size() != num_cols_) {
        throw std::runtime_error(
            "bin_writer: column count mismatch on repeated header call. "
            "Expected " + std::to_string(num_cols_) + 
            ", got " + std::to_string(names.size()));
      }
      return;  // Ignore duplicate header with same column count
    }
    names_ = names;
    num_cols_ = names.size();
    header_written_ = true;
  }

  /**
   * Write a row of samples. Called once per iteration.
   * 
   * @param state Vector of parameter values for this iteration
   */
  void operator()(const std::vector<double>& state) override {
    if (state.empty()) {
      return;
    }
    
    data_stream_.write(
        reinterpret_cast<const char*>(state.data()),
        static_cast<std::streamsize>(state.size() * sizeof(double)));
    
    if (!data_stream_) {
      throw std::runtime_error("bin_writer: failed to write sample data");
    }
    
    ++num_rows_;
  }

  /**
   * Handle text messages. Ignored for binary output.
   */
  void operator()(const std::string& /*message*/) override {
  }
  
  /**
   * Handle blank line markers. Ignored for binary output.
   */
  void operator()() override {
  }

  /**
   * Finalize output. Closes data stream and writes metadata file.
   * Must be called after sampling completes.
   */
  void finalize() {
    if (finalized_) {
      return;
    }

    if (data_stream_.is_open()) {
      data_stream_.close();
    }

    write_metadata();
    finalized_ = true;

    std::cerr << "Binary output: " << num_rows_ << " samples, "
              << num_cols_ << " parameters written to "
              << base_path_ + ".bin" << std::endl;
  }

  std::size_t num_rows() const { return num_rows_; }
  std::size_t num_cols() const { return num_cols_; }
  bool is_finalized() const { return finalized_; }

  private:
  void write_metadata() {
    std::ofstream meta(base_path_ + ".meta", std::ios::trunc);
    if (!meta) {
      throw std::runtime_error("Cannot open metadata file: " 
                               + base_path_ + ".meta");
    }

    meta << "version=1\n";
    
    meta << "rows=" << num_rows_ << "\n";
    meta << "cols=" << num_cols_ << "\n";
    
    meta << "type=float64\n";
    meta << "endian=little\n";
    meta << "order=row_major\n";
    
    for (std::size_t i = 0; i < names_.size(); ++i) {
      meta << "name." << i << "=" << names_[i] << "\n";
    }
    
    if (!meta) {
      throw std::runtime_error("Failed to write metadata file");
    }
  }
};

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_BIN_WRITER_HPP
