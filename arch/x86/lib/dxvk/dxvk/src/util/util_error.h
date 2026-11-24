#pragma once

#include <string>

namespace dxvk {

  /**
   * \brief DXVK error
   *
   * A generic exception class that stores a
   * message. Exceptions should be logged.
   */
  class DxvkError {

  public:

    DxvkError() { }
    DxvkError(std::string&& message)
    : m_message(std::move(message)) { }

    const std::string& message() const {
      return m_message;
    }

#ifdef __OSITO_K__
    /* OsitoK: build with -fno-exceptions. `throw DxvkError(msg)` statements
     * in DXVK .cpp files are rewritten (W5.2) to call this noreturn helper
     * with the original constructor argument. Two overloads cover both
     * string-literal sites and `str::format(...)` (returns std::string)
     * sites without forcing a sed-time .c_str() ritual. */
    [[noreturn]] static void abort_ositok(const char *msg);
    [[noreturn]] static void abort_ositok(const std::string& msg);
    [[noreturn]] static void abort_ositok();
#endif

  private:

    std::string m_message;

  };

}
