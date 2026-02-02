#ifndef CMDSTAN_ARGUMENTS_ARG_CHECKSUM_HPP
#define CMDSTAN_ARGUMENTS_ARG_CHECKSUM_HPP

#include <cmdstan/arguments/singleton_argument.hpp>

namespace cmdstan {

class arg_checksum : public bool_argument {
 public:
  arg_checksum() : bool_argument() {
    _name = "checksum";
    _description = "Include CRC32 checksum in stanbin output";
    _validity = "true, false";
    _default = "true";
    _default_value = true;
    _value = _default_value;
  }
};

}  // namespace cmdstan

#endif  // CMDSTAN_ARGUMENTS_ARG_CHECKSUM_HPP
