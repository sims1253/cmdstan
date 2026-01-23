#ifndef CMDSTAN_IO_BEVE_WRITER_HPP
#define CMDSTAN_IO_BEVE_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <glaze/glaze.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstddef>

namespace cmdstan {
namespace io {

/**
 * Structure to hold Stan output data for BEVE serialization.
 * Uses column-major layout for efficient data accumulation.
 */
struct StanOutput {
  std::vector<std::string> names;              // column names
  std::size_t rows = 0;                        // number of samples
  std::size_t cols = 0;                        // number of columns
  std::vector<std::vector<double>> data;       // data[col][row] - column-major

  // Glaze reflection for automatic serialization
  static constexpr auto glz_members = std::make_tuple(
    &StanOutput::names,
    &StanOutput::rows,
    &StanOutput::cols,
    &StanOutput::data
  );
};

/**
 * BEVE binary writer for Stan MCMC output using the Glaze library.
 *
 * Accumulates all samples in memory and writes them to a .beve file
 * upon finalization. BEVE provides a compact binary format with
 * self-describing metadata.
 */
class bevel_writer : public stan::callbacks::writer {
 private:
  std::string base_path_;
  StanOutput output_;
  std::size_t header_written_ = 0;
  bool finalized_ = false;

 public:
  /**
   * Construct a BEVE writer.
   *
   * @param base_path Path without extension. Will create {base_path}.beve file.
   */
  explicit bevel_writer(const std::string& base_path)
      : base_path_(base_path) {}

  /**
   * Destructor ensures finalize() is called.
   */
  ~bevel_writer() override {
    try {
      if (!finalized_) {
        finalize();
      }
    } catch (...) {
      // Destructors must not throw
    }
  }

  // Non-copyable
  bevel_writer(const bevel_writer&) = delete;
  bevel_writer& operator=(const bevel_writer&) = delete;

  // Movable
  bevel_writer(bevel_writer&& other) noexcept
      : base_path_(std::move(other.base_path_)),
        output_(std::move(other.output_)),
        header_written_(other.header_written_),
        finalized_(other.finalized_) {
    other.finalized_ = true;
  }

  bevel_writer& operator=(bevel_writer&& other) noexcept {
    if (this != &other) {
      if (!finalized_) {
        finalize();
      }
      base_path_ = std::move(other.base_path_);
      output_ = std::move(other.output_);
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
    if (header_written_ > 0) {
      // Guard against multiple calls
      if (names.size() != output_.cols) {
        throw std::runtime_error(
            "bevel_writer: column count mismatch on repeated header call. "
            "Expected " + std::to_string(output_.cols) +
            ", got " + std::to_string(names.size()));
      }
      return;
    }
    output_.names = names;
    output_.cols = names.size();

    // Pre-allocate column vectors for column-major storage
    if (output_.cols > 0) {
      output_.data.resize(output_.cols);
      for (auto& col : output_.data) {
        col.reserve(1024);  // Initial capacity hint
      }
    }

    header_written_ = 1;
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

    if (!header_written_) {
      throw std::runtime_error(
          "bevel_writer: received sample data before column names");
    }

    if (state.size() != output_.cols) {
      throw std::runtime_error(
          "bevel_writer: sample size mismatch. Expected " +
          std::to_string(output_.cols) + ", got " +
          std::to_string(state.size()));
    }

    // Accumulate in column-major format: output_.data[col][row]
    for (std::size_t col = 0; col < output_.cols; ++col) {
      output_.data[col].push_back(state[col]);
    }

    ++output_.rows;
  }

  /**
   * Handle text messages. Ignored for BEVE output.
   */
  void operator()(const std::string& /*message*/) override {
  }

  /**
   * Handle blank line markers. Ignored for BEVE output.
   */
  void operator()() override {
  }

  /**
   * Finalize output. Serializes data to BEVE format and writes to file.
   * Must be called after sampling completes.
   */
  void finalize() {
    if (finalized_) {
      return;
    }

    if (!header_written_) {
      throw std::runtime_error(
          "bevel_writer: finalize called without prior header");
    }

    // Validate we have data
    if (output_.rows == 0) {
      std::cerr << "BEVE output: No samples collected, writing empty file to "
                << base_path_ << ".beve" << std::endl;
    }

    // Serialize to BEVE and write to file
    std::string file_path = base_path_ + ".beve";

    std::vector<std::byte> buffer;
    auto result = glz::write_beve(output_, buffer);

    if (result) {
      throw std::runtime_error(
          "bevel_writer: failed to serialize BEVE data: " +
          std::to_string(static_cast<int>(result.ec)));
    }

    // Write buffer to file
    std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw std::runtime_error(
          "bevel_writer: cannot open output file: " + file_path);
    }

    file.write(reinterpret_cast<const char*>(buffer.data()),
               static_cast<std::streamsize>(buffer.size()));

    if (!file) {
      throw std::runtime_error(
          "bevel_writer: failed to write to file: " + file_path);
    }

    file.close();

    finalized_ = true;

    std::cerr << "BEVE output: " << output_.rows << " samples, "
              << output_.cols << " parameters written to "
              << file_path << std::endl;
  }

  std::size_t num_rows() const { return output_.rows; }
  std::size_t num_cols() const { return output_.cols; }
  bool is_finalized() const { return finalized_; }

  /**
   * Get the accumulated output data (for testing or advanced use).
   * Only valid before finalize() is called.
   */
  const StanOutput& output() const { return output_; }
};

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_BEVE_WRITER_HPP
