#ifndef CMDSTAN_ARGUMENTS_ARG_COMPRESSION_HPP
#define CMDSTAN_ARGUMENTS_ARG_COMPRESSION_HPP

#include <cmdstan/arguments/singleton_argument.hpp>
#include <string>

namespace cmdstan {

class arg_compression : public string_argument {
 public:
  arg_compression() : string_argument() {
    _name = "compression";
    _description = "Compression algorithm for stanbin format";
    _validity = "none, lz4, zstd";
    _default = "none";
    _default_value = "none";
    _constrained = true;
    _good_value = "none";
    _value = _default_value;
  }

  bool is_valid(std::string value) override {
    return value == "none" || value == "lz4" || value == "zstd";
  }
};

}  // namespace cmdstan

#endif  // CMDSTAN_ARGUMENTS_ARG_COMPRESSION_HPP
