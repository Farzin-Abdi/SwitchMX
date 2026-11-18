// The translations: one file per language (lang_fa.cpp, ...), each a list of {English, translation}.
#pragma once
#include <cstddef>

namespace lang {
// The rows for a language code, or nullptr (n = 0) for English and unknown codes.
const char* const (*table(const char* code, size_t& n))[2];
}  // namespace lang
