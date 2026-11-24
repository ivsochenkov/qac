#include "qac/jamspell_fixer.hpp"

#include <stdexcept>
#include <string>

#include <jamspell/spell_corrector.hpp>

#include "qac/utf8.hpp"

namespace qac {

static_assert(sizeof(wchar_t) == 4, "JamSpell is driven with UTF-32 wide strings");

struct JamSpellFixer::Impl {
  NJamSpell::TSpellCorrector corrector;
};

JamSpellFixer::JamSpellFixer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
JamSpellFixer::~JamSpellFixer() = default;

std::unique_ptr<JamSpellFixer> JamSpellFixer::load(const std::string& model_path) {
  try {
    auto impl = std::make_unique<Impl>();
    if (!impl->corrector.LoadLangModel(model_path))
      throw std::runtime_error("the model file is missing or not a JamSpell model");
    auto fixer = std::unique_ptr<JamSpellFixer>(new JamSpellFixer(std::move(impl)));
    fixer->fix("test");  // fail now, not on the first request
    return fixer;
  } catch (const std::exception& e) {
    throw std::runtime_error(
        "cannot load the JamSpell model '" + model_path + "': " + e.what() +
        ". Note: the JamSpell fork needs the ru_RU.UTF-8 locale to be installed "
        "(Debian/Ubuntu: locale-gen ru_RU.UTF-8; Nix: set LOCALE_ARCHIVE to "
        "glibcLocales, which the provided flake does).");
  }
}

std::string JamSpellFixer::fix(const std::string& utf8_fragment) const {
  std::u32string cps;
  if (!utf8::decode(utf8_fragment, cps)) return utf8_fragment;
  const std::wstring wide(cps.begin(), cps.end());
  const std::wstring fixed = impl_->corrector.FixFragment(wide);
  return utf8::encode(std::u32string(fixed.begin(), fixed.end()));
}

}  // namespace qac
