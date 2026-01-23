#ifndef CMDSTAN_IO_ARROW_WRITER_HPP
#define CMDSTAN_IO_ARROW_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>
#include <arrow/ipc/writer.h>
#include <iostream>
#include <vector>
#include <string>
#include <memory>
#include <stdexcept>

namespace cmdstan {
namespace io {

/**
 * Format enumeration for Arrow-based output formats.
 */
enum class ArrowFormat { PARQUET, FEATHER };

/**
 * Arrow writer for Stan MCMC output.
 *
 * Writes samples to either Parquet or Feather (IPC) format files.
 * All data is buffered in memory and written on finalize().
 *
 */
class arrow_writer : public stan::callbacks::writer {
 private:
  std::string file_path_;
  ArrowFormat format_;
  std::vector<std::string> names_;
  std::vector<std::vector<double>> columns_;
  std::size_t num_rows_ = 0;
  bool header_received_ = false;
  bool finalized_ = false;

 public:
  /**
   * Construct an Arrow writer.
   *
   * @param file_path Path to the output file (.parquet or .feather extension)
   * @param format Output format (PARQUET or FEATHER)
   */
  arrow_writer(const std::string& file_path, ArrowFormat format)
      : file_path_(file_path), format_(format) {
    // Reserve initial capacity for columns (will grow as needed)
  }

  /**
   * Destructor ensures finalize() is called.
   */
  ~arrow_writer() override {
    try {
      if (!finalized_) {
        finalize();
      }
    } catch (...) {
      // Destructors must not throw
    }
  }

  // Non-copyable
  arrow_writer(const arrow_writer&) = delete;
  arrow_writer& operator=(const arrow_writer&) = delete;

  // Movable
  arrow_writer(arrow_writer&& other) noexcept
      : file_path_(std::move(other.file_path_)),
        format_(other.format_),
        names_(std::move(other.names_)),
        columns_(std::move(other.columns_)),
        num_rows_(other.num_rows_),
        header_received_(other.header_received_),
        finalized_(other.finalized_) {
    other.finalized_ = true;
  }

  arrow_writer& operator=(arrow_writer&& other) noexcept {
    if (this != &other) {
      if (!finalized_) {
        finalize();
      }
      file_path_ = std::move(other.file_path_);
      format_ = other.format_;
      names_ = std::move(other.names_);
      columns_ = std::move(other.columns_);
      num_rows_ = other.num_rows_;
      header_received_ = other.header_received_;
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
    if (header_received_) {
      // Guard against multiple calls
      if (names.size() != names_.size()) {
        throw std::runtime_error(
            "arrow_writer: column count mismatch on repeated header call. "
            "Expected " + std::to_string(names_.size()) +
            ", got " + std::to_string(names.size()));
      }
      return;  // Ignore duplicate header with same column count
    }

    names_ = names;
    const std::size_t num_cols = names.size();

    // Initialize column vectors
    columns_.resize(num_cols);

    header_received_ = true;
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

    if (!header_received_) {
      throw std::runtime_error(
          "arrow_writer: received data before column names");
    }

    if (state.size() != columns_.size()) {
      throw std::runtime_error(
          "arrow_writer: column count mismatch. Expected " +
          std::to_string(columns_.size()) + ", got " +
          std::to_string(state.size()));
    }

    // Append to column vectors
    for (std::size_t i = 0; i < state.size(); ++i) {
      columns_[i].push_back(state[i]);
    }

    ++num_rows_;
  }

  /**
   * Handle text messages. Ignored for Arrow output.
   */
  void operator()(const std::string& /*message*/) override {
  }

  /**
   * Handle blank line markers. Ignored for Arrow output.
   */
  void operator()() override {
  }

  /**
   * Finalize output. Writes the file and prints summary to stderr.
   * Must be called after sampling completes.
   */
  void finalize() {
    if (finalized_) {
      return;
    }

    if (!header_received_) {
      // No data was written
      finalized_ = true;
      return;
    }

    arrow::Status status;

    if (format_ == ArrowFormat::PARQUET) {
      status = write_parquet();
    } else {
      status = write_feather();
    }

    if (!status.ok()) {
      throw std::runtime_error("arrow_writer: failed to write file: " +
                               status.ToString());
    }

    std::cerr << "Arrow output: " << num_rows_ << " samples, "
              << names_.size() << " parameters written to "
              << file_path_ << std::endl;

    finalized_ = true;
  }

  std::size_t num_rows() const { return num_rows_; }
  std::size_t num_cols() const { return names_.size(); }
  bool is_finalized() const { return finalized_; }

 private:
  /**
   * Write output in Parquet format.
   */
  arrow::Status write_parquet() {
    // Build Arrow schema
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(names_.size());
    for (const auto& name : names_) {
      fields.push_back(arrow::field(name, arrow::float64()));
    }
    auto schema = arrow::schema(fields);

    // Build Arrow arrays from column data
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(columns_.size());

    for (const auto& col : columns_) {
      arrow::DoubleBuilder builder;
      ARROW_RETURN_NOT_OK(builder.AppendValues(col));
      std::shared_ptr<arrow::Array> array;
      ARROW_RETURN_NOT_OK(builder.Finish(&array));
      arrays.push_back(array);
    }

    // Create record batch
    auto record_batch =
        arrow::RecordBatch::Make(schema, num_rows_, arrays);

    // Open file for writing
    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    ARROW_ASSIGN_OR_RAISE(outfile, arrow::io::FileOutputStream::Open(file_path_));

    // Parquet writer properties
    parquet::WriterProperties::Builder builder;
    builder.compression(arrow::Compression::GZIP);
    std::shared_ptr<parquet::WriterProperties> props = builder.build();

    // Arrow writer properties
    parquet::ArrowWriterProperties::Builder arrow_props;
    arrow_props.store_schema();
    std::shared_ptr<parquet::ArrowWriterProperties> arrow_props_ptr =
        arrow_props.build();

    // Create table from record batch
    auto table = arrow::Table::FromRecordBatches({record_batch}).ValueOrDie();

    // Write using FileWriter (Arrow 22.0.0 Result-based API)
    ARROW_ASSIGN_OR_RAISE(
        auto writer,
        parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(),
                                          outfile, props, arrow_props_ptr));
    ARROW_RETURN_NOT_OK(writer->WriteTable(*table, table->num_rows()));
    ARROW_RETURN_NOT_OK(writer->Close());

    return arrow::Status::OK();
  }

  /**
   * Write output in Feather format (IPC columnar).
   */
  arrow::Status write_feather() {
    // Build Arrow schema
    std::vector<std::shared_ptr<arrow::Field>> fields;
    fields.reserve(names_.size());
    for (const auto& name : names_) {
      fields.push_back(arrow::field(name, arrow::float64()));
    }
    auto schema = arrow::schema(fields);

    // Build Arrow arrays from column data
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    arrays.reserve(columns_.size());

    for (const auto& col : columns_) {
      arrow::DoubleBuilder builder;
      ARROW_RETURN_NOT_OK(builder.AppendValues(col));
      std::shared_ptr<arrow::Array> array;
      ARROW_RETURN_NOT_OK(builder.Finish(&array));
      arrays.push_back(array);
    }

    // Create table
    auto table = arrow::Table::Make(schema, arrays, num_rows_);

    // Open file for writing
    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    ARROW_ASSIGN_OR_RAISE(outfile, arrow::io::FileOutputStream::Open(file_path_));

    // Write Feather file using IPC file writer (Feather v2 = IPC format)
    arrow::ipc::IpcWriteOptions write_options;
    ARROW_ASSIGN_OR_RAISE(
        auto writer,
        arrow::ipc::MakeFileWriter(outfile, schema, write_options));
    ARROW_RETURN_NOT_OK(writer->WriteTable(*table));
    ARROW_RETURN_NOT_OK(writer->Close());

    ARROW_RETURN_NOT_OK(outfile->Close());

    return arrow::Status::OK();
  }
};

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_ARROW_WRITER_HPP
