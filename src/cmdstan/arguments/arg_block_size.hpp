#ifndef CMDSTAN_ARGUMENTS_ARG_BLOCK_SIZE_HPP
#define CMDSTAN_ARGUMENTS_ARG_BLOCK_SIZE_HPP

#include <cmdstan/arguments/singleton_argument.hpp>
#include <string>

namespace cmdstan {

class arg_block_size : public int_argument {
 public:
  arg_block_size() : int_argument() {
    _name = "block_size";
    _description = "Rows per compression block (stanbin format)";
    _validity = "1 <= block_size";
    _default = "64";
    _default_value = 64;
    _value = _default_value;
  }

  bool is_valid(int value) { return value >= 1; }
};

}  // namespace cmdstan

#endif  // CMDSTAN_ARGUMENTS_ARG_BLOCK_SIZE_HPP
