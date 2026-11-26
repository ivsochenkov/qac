// SpellFixer backed by the JamSpell fork (third_party/jamspell).
//
// JamSpell is used purely as a spell corrector: a model is loaded and
// FixFragment() is called. None of its code or definitions are reused.
// The fork's headers are large, so they are kept out of this interface and out of
// every translation unit but jamspell_fixer.cpp. (JamSpell and this project are built
// from the same copy of hat-trie, array-hash and cereal; see cmake/jamspell/CMakeLists.txt.)
#pragma once

#include <memory>
#include <string>

#include "qac/spell_fixer.hpp"

namespace qac {

class JamSpellFixer final : public SpellFixer {
 public:
  // Throws std::runtime_error with an actionable message on failure.
  static std::unique_ptr<JamSpellFixer> load(const std::string& model_path);

  ~JamSpellFixer() override;

  // Corrects a fragment of complete words (UTF-8 in, UTF-8 out). Input that is not
  // valid UTF-8 is returned unchanged.
  std::string fix(const std::string& utf8_fragment) const override;

 private:
  struct Impl;
  explicit JamSpellFixer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace qac
