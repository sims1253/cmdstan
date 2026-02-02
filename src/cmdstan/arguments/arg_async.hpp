#ifndef CMDSTAN_ARGUMENTS_ARG_ASYNC_HPP
#define CMDSTAN_ARGUMENTS_ARG_ASYNC_HPP

#include <cmdstan/arguments/singleton_argument.hpp>

namespace cmdstan {

class arg_async : public bool_argument {
 public:
  arg_async() : bool_argument() {
    _name = "async";
    _description = "Use async I/O for stanbin format (background writer thread)";
    _validity = "true, false";
    _default = "true";
    _default_value = true;
    _value = _default_value;
  }
};

}  // namespace cmdstan

#endif  // CMDSTAN_ARGUMENTS_ARG_ASYNC_HPP
