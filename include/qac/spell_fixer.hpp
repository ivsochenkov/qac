// The spell-correction hook of the engine. Deliberately free of any other project
// header, so implementations may live in translation units with unrelated
// dependencies (see jamspell_fixer.cpp).
#pragma once

#include <string>

namespace qac {

// Optional spell correction hook. Implementations must be thread-safe.
class SpellFixer {
 public:
  virtual ~SpellFixer() = default;
  // `utf8_fragment` holds complete words separated by spaces; returns the
  // corrected text (it may be identical).
  virtual std::string fix(const std::string& utf8_fragment) const = 0;
};

}  // namespace qac
