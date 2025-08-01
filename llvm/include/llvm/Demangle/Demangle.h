//===--- Demangle.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEMANGLE_DEMANGLE_H
#define LLVM_DEMANGLE_DEMANGLE_H

#include "DemangleConfig.h"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace llvm {
/// This is a llvm local version of __cxa_demangle. Other than the name and
/// being in the llvm namespace it is identical.
///
/// The mangled_name is demangled into buf and returned. If the buffer is not
/// large enough, realloc is used to expand it.
///
/// The *status will be set to a value from the following enumeration
enum : int {
  demangle_unknown_error = -4,
  demangle_invalid_args = -3,
  demangle_invalid_mangled_name = -2,
  demangle_memory_alloc_failure = -1,
  demangle_success = 0,
};

/// Returns a non-NULL pointer to a NUL-terminated C style string
/// that should be explicitly freed, if successful. Otherwise, may return
/// nullptr if mangled_name is not a valid mangling or is nullptr.
DEMANGLE_ABI char *itaniumDemangle(std::string_view mangled_name,
                                   bool ParseParams = true);

enum MSDemangleFlags {
  MSDF_None = 0,
  MSDF_DumpBackrefs = 1 << 0,
  MSDF_NoAccessSpecifier = 1 << 1,
  MSDF_NoCallingConvention = 1 << 2,
  MSDF_NoReturnType = 1 << 3,
  MSDF_NoMemberType = 1 << 4,
  MSDF_NoVariableType = 1 << 5,
};

/// Demangles the Microsoft symbol pointed at by mangled_name and returns it.
/// Returns a pointer to the start of a null-terminated demangled string on
/// success, or nullptr on error.
/// If n_read is non-null and demangling was successful, it receives how many
/// bytes of the input string were consumed.
/// status receives one of the demangle_ enum entries above if it's not nullptr.
/// Flags controls various details of the demangled representation.
DEMANGLE_ABI char *microsoftDemangle(std::string_view mangled_name,
                                     size_t *n_read, int *status,
                                     MSDemangleFlags Flags = MSDF_None);

DEMANGLE_ABI std::optional<size_t>
getArm64ECInsertionPointInMangledName(std::string_view MangledName);

// Demangles a Rust v0 mangled symbol.
DEMANGLE_ABI char *rustDemangle(std::string_view MangledName);

// Demangles a D mangled symbol.
DEMANGLE_ABI char *dlangDemangle(std::string_view MangledName);

/// Attempt to demangle a string using different demangling schemes.
/// The function uses heuristics to determine which demangling scheme to use.
/// \param MangledName - reference to string to demangle.
/// \returns - the demangled string, or a copy of the input string if no
/// demangling occurred.
DEMANGLE_ABI std::string demangle(std::string_view MangledName);

DEMANGLE_ABI bool nonMicrosoftDemangle(std::string_view MangledName,
                                       std::string &Result,
                                       bool CanHaveLeadingDot = true,
                                       bool ParseParams = true);

/// "Partial" demangler. This supports demangling a string into an AST
/// (typically an intermediate stage in itaniumDemangle) and querying certain
/// properties or partially printing the demangled name.
struct ItaniumPartialDemangler {
  DEMANGLE_ABI ItaniumPartialDemangler();

  DEMANGLE_ABI ItaniumPartialDemangler(ItaniumPartialDemangler &&Other);
  DEMANGLE_ABI ItaniumPartialDemangler &
  operator=(ItaniumPartialDemangler &&Other);

  /// Demangle into an AST. Subsequent calls to the rest of the member functions
  /// implicitly operate on the AST this produces.
  /// \return true on error, false otherwise
  DEMANGLE_ABI bool partialDemangle(const char *MangledName);

  /// Just print the entire mangled name into Buf. Buf and N behave like the
  /// second and third parameters to __cxa_demangle.
  DEMANGLE_ABI char *finishDemangle(char *Buf, size_t *N) const;

  /// See \ref finishDemangle
  ///
  /// \param[in] OB A llvm::itanium_demangle::OutputBuffer that the demangled
  /// name will be printed into.
  ///
  DEMANGLE_ABI char *finishDemangle(void *OB) const;

  /// Get the base name of a function. This doesn't include trailing template
  /// arguments, ie for "a::b<int>" this function returns "b".
  DEMANGLE_ABI char *getFunctionBaseName(char *Buf, size_t *N) const;

  /// Get the context name for a function. For "a::b::c", this function returns
  /// "a::b".
  DEMANGLE_ABI char *getFunctionDeclContextName(char *Buf, size_t *N) const;

  /// Get the entire name of this function.
  DEMANGLE_ABI char *getFunctionName(char *Buf, size_t *N) const;

  /// Get the parameters for this function.
  DEMANGLE_ABI char *getFunctionParameters(char *Buf, size_t *N) const;
  DEMANGLE_ABI char *getFunctionReturnType(char *Buf, size_t *N) const;

  /// If this function has any cv or reference qualifiers. These imply that
  /// the function is a non-static member function.
  DEMANGLE_ABI bool hasFunctionQualifiers() const;

  /// If this symbol describes a constructor or destructor.
  DEMANGLE_ABI bool isCtorOrDtor() const;

  /// If this symbol describes a function.
  DEMANGLE_ABI bool isFunction() const;

  /// If this symbol describes a variable.
  DEMANGLE_ABI bool isData() const;

  /// If this symbol is a <special-name>. These are generally implicitly
  /// generated by the implementation, such as vtables and typeinfo names.
  DEMANGLE_ABI bool isSpecialName() const;

  DEMANGLE_ABI ~ItaniumPartialDemangler();

private:
  void *RootNode;
  void *Context;
};
} // namespace llvm

#endif
