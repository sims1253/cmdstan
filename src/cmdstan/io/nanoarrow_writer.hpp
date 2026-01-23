#ifndef CMDSTAN_IO_NANOARROW_WRITER_HPP
#define CMDSTAN_IO_NANOARROW_WRITER_HPP

#include <stan/callbacks/writer.hpp>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <cstddef>
#include <cstdio>

// nanoarrow is C, so extern "C"
extern "C" {
#include "nanoarrow.h"
#include "nanoarrow_ipc.h"
}

namespace cmdstan {
namespace io {

/**
 * Arrow IPC writer for Stan MCMC output using nanoarrow (lightweight).
 *
 * Writes samples to Arrow IPC file format (.arrows or .arrow).
 * This is a lightweight alternative to the full Arrow C++ library.
 */
class nanoarrow_writer : public stan::callbacks::writer {
 private:
  std::string file_path_;
  std::vector<std::string> names_;
  std::vector<std::vector<double>> columns_;  // column-major
  std::size_t num_rows_ = 0;
  bool header_received_ = false;
  bool finalized_ = false;

 public:
  explicit nanoarrow_writer(const std::string& file_path)
      : file_path_(file_path) {}

  ~nanoarrow_writer() override {
    try {
      if (!finalized_) {
        finalize();
      }
    } catch (...) {
      // Destructors must not throw
    }
  }

  // Non-copyable, movable
  nanoarrow_writer(const nanoarrow_writer&) = delete;
  nanoarrow_writer& operator=(const nanoarrow_writer&) = delete;

  nanoarrow_writer(nanoarrow_writer&& other) noexcept
      : file_path_(std::move(other.file_path_)),
        names_(std::move(other.names_)),
        columns_(std::move(other.columns_)),
        num_rows_(other.num_rows_),
        header_received_(other.header_received_),
        finalized_(other.finalized_) {
    other.finalized_ = true;
  }

  nanoarrow_writer& operator=(nanoarrow_writer&& other) noexcept {
    if (this != &other) {
      if (!finalized_) {
        finalize();
      }
      file_path_ = std::move(other.file_path_);
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
            "nanoarrow_writer: column count mismatch on repeated header call. "
            "Expected " + std::to_string(names_.size()) +
            ", got " + std::to_string(names.size()));
      }
      return;  // Ignore duplicate header with same column count
    }
    names_ = names;
    columns_.resize(names.size());
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
      throw std::runtime_error("nanoarrow_writer: data before header");
    }
    if (state.size() != columns_.size()) {
      throw std::runtime_error(
          "nanoarrow_writer: column count mismatch. Expected " +
          std::to_string(columns_.size()) + ", got " +
          std::to_string(state.size()));
    }
    for (std::size_t i = 0; i < state.size(); ++i) {
      columns_[i].push_back(state[i]);
    }
    ++num_rows_;
  }

  /**
   * Handle text messages. Ignored for Arrow output.
   */
  void operator()(const std::string&) override {}

  /**
   * Handle blank line markers. Ignored for Arrow output.
   */
  void operator()() override {}

  /**
   * Finalize output. Writes the Arrow IPC file.
   * Must be called after sampling completes.
   */
  void finalize() {
    if (finalized_) {
      return;
    }

    if (!header_received_) {
      // No data was written, but still mark as finalized
      finalized_ = true;
      return;
    }

    ArrowError error;

    // Build schema
    ArrowSchema schema;
    ArrowSchemaInit(&schema);
    ArrowSchemaSetTypeStruct(&schema, columns_.size());

    // Allocate children array - REQUIRED before accessing schema.children
    ArrowSchemaAllocateChildren(&schema, columns_.size());

    // Configure each child schema (column)
    for (std::size_t i = 0; i < names_.size(); ++i) {
      ArrowSchemaInit(schema.children[i]);
      ArrowSchemaSetType(schema.children[i], NANOARROW_TYPE_DOUBLE);
      ArrowSchemaSetName(schema.children[i], names_[i].c_str());
    }

    // Build array from schema
    ArrowArray array;
    ArrowArrayInitFromSchema(&array, &schema, &error);

    // Start appending - this initializes the array for appending
    ArrowArrayStartAppending(&array);

    // Append data row by row (struct arrays expect row-by-row appending)
    for (std::size_t row = 0; row < num_rows_; ++row) {
      // For each column, append the value
      for (std::size_t col = 0; col < columns_.size(); ++col) {
        ArrowErrorCode result = ArrowArrayAppendDouble(array.children[col], columns_[col][row]);
        if (result != NANOARROW_OK) {
          ArrowSchemaRelease(&schema);
          ArrowArrayRelease(&array);
          throw std::runtime_error("nanoarrow_writer: failed to append double value");
        }
      }
      // After appending all columns for this row, finish the struct row
      ArrowArrayFinishElement(&array);
    }

    // Finish building the array - this sets the final length
    ArrowErrorCode result = ArrowArrayFinishBuildingDefault(&array, &error);
    if (result != NANOARROW_OK) {
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to build array: ") +
                               ArrowErrorMessage(&error));
    }

    // Create ArrowArrayView for IPC writing
    ArrowArrayView array_view;
    result = ArrowArrayViewInitFromSchema(&array_view, &schema, &error);
    if (result != NANOARROW_OK) {
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to init array view: ") +
                               ArrowErrorMessage(&error));
    }

    result = ArrowArrayViewSetArray(&array_view, &array, &error);
    if (result != NANOARROW_OK) {
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to set array view: ") +
                               ArrowErrorMessage(&error));
    }

    // Open file and create IPC output stream
    FILE* file = fopen(file_path_.c_str(), "wb");
    if (!file) {
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error("nanoarrow_writer: cannot open output file: " + file_path_);
    }

    ArrowIpcOutputStream output;
    result = ArrowIpcOutputStreamInitFile(&output, file, 1);  // 1 = close_on_release
    if (result != NANOARROW_OK) {
      fclose(file);
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error("nanoarrow_writer: failed to init IPC output stream");
    }

    // Create IPC writer
    ArrowIpcWriter writer;
    result = ArrowIpcWriterInit(&writer, &output);
    if (result != NANOARROW_OK) {
      if (output.release != NULL) {
        output.release(&output);
      }
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error("nanoarrow_writer: failed to init IPC writer");
    }

    // Start file (writes magic bytes)
    result = ArrowIpcWriterStartFile(&writer, &error);
    if (result != NANOARROW_OK) {
      ArrowIpcWriterReset(&writer);
      if (output.release != NULL) {
        output.release(&output);
      }
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to start file: ") +
                               ArrowErrorMessage(&error));
    }

    // Write schema
    result = ArrowIpcWriterWriteSchema(&writer, &schema, &error);
    if (result != NANOARROW_OK) {
      ArrowIpcWriterReset(&writer);
      if (output.release != NULL) {
        output.release(&output);
      }
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to write schema: ") +
                               ArrowErrorMessage(&error));
    }

    // Write array
    result = ArrowIpcWriterWriteArrayView(&writer, &array_view, &error);
    if (result != NANOARROW_OK) {
      ArrowIpcWriterReset(&writer);
      if (output.release != NULL) {
        output.release(&output);
      }
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to write array: ") +
                               ArrowErrorMessage(&error));
    }

    // Finalize file (writes footer)
    result = ArrowIpcWriterFinalizeFile(&writer, &error);
    if (result != NANOARROW_OK) {
      ArrowIpcWriterReset(&writer);
      if (output.release != NULL) {
        output.release(&output);
      }
      ArrowArrayViewReset(&array_view);
      ArrowSchemaRelease(&schema);
      ArrowArrayRelease(&array);
      throw std::runtime_error(std::string("nanoarrow_writer: failed to finalize file: ") +
                               ArrowErrorMessage(&error));
    }

    // Cleanup - order matters: writer, output, array_view, array, schema
    ArrowIpcWriterReset(&writer);
    if (output.release != NULL) {
      output.release(&output);
    }
    ArrowArrayViewReset(&array_view);
    ArrowArrayRelease(&array);
    ArrowSchemaRelease(&schema);

    std::cerr << "Arrow IPC output: " << num_rows_ << " samples, "
              << names_.size() << " parameters written to "
              << file_path_ << std::endl;

    finalized_ = true;
  }

  std::size_t num_rows() const { return num_rows_; }
  std::size_t num_cols() const { return names_.size(); }
  bool is_finalized() const { return finalized_; }
};

}  // namespace io
}  // namespace cmdstan

#endif  // CMDSTAN_IO_NANOARROW_WRITER_HPP
