#ifndef CMDSTAN_ARGUMENTS_ARG_FORMAT_HPP
#define CMDSTAN_ARGUMENTS_ARG_FORMAT_HPP

#include <cmdstan/arguments/singleton_argument.hpp>
#include <string>

namespace cmdstan {

class arg_format : public string_argument {
 public:
  arg_format() : string_argument() {
    _name = "format";
    _description = "Output file format";
    _validity = "csv, bin";
    _default = "csv";
    _default_value = "csv";
    _constrained = true;
    _good_value = "csv";
    _value = _default_value;
  }

  bool is_valid(std::string value) override {
    return value == "csv" || value == "bin";
  }
};

}  // namespace cmdstan

#endif  // CMDSTAN_ARGUMENTS_ARG_FORMAT_HPP
