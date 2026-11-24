// Test helper: trains a JamSpell model through the fork's public API. It lives in
// its own translation unit because the fork's headers must not be mixed with this
// project's (see cmake/jamspell/CMakeLists.txt).
#pragma once
#include <string>

namespace qac::test {
bool train_jamspell_model(const std::string& text_file, const std::string& dict_file,
                          const std::string& alphabet_file, const std::string& model_file);
}
