#include "jamspell_train.hpp"

#include <jamspell/spell_corrector.hpp>

namespace qac::test {
bool train_jamspell_model(const std::string& text_file, const std::string& dict_file,
                          const std::string& alphabet_file, const std::string& model_file) {
  NJamSpell::TSpellCorrector trainer;
  return trainer.TrainLangModel(text_file, dict_file, alphabet_file, model_file);
}
}  // namespace qac::test
