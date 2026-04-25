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
     * in DXVK .cpp files are rewritten by a helper macro in
     * dxbc_include.h / dxvk_include.h that funnels through this noreturn
     * member. Defined in ositok_dxvk_throw.cpp or cxx_stubs.c. */
    [[noreturn]] static void abort_ositok(const char *msg);
#endif

  private:

    std::string m_message;

  };

}
