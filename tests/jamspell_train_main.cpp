// Build-time helper: trains the small JamSpell model the integration tests load.
// Usage: qac_jamspell_train_model <text> <dict> <alphabet> <model>
//
// Training needs several GB of RAM, so it is done once at build time rather than
// once per test process. A host without the ru_RU.UTF-8 locale (which the fork
// requires) is not an error: no model is written and the tests skip themselves.
#include <iostream>
#include <locale>
#include <stdexcept>

#include "jamspell_train.hpp"

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: " << argv[0] << " <text> <dict> <alphabet> <model>\n";
    return 2;
  }
  try {
    std::locale probe("ru_RU.UTF-8");
  } catch (const std::exception&) {
    std::cerr << "note: ru_RU.UTF-8 is not installed; skipping the JamSpell test model\n";
    return 0;
  }
  try {
    if (!qac::test::train_jamspell_model(argv[1], argv[2], argv[3], argv[4])) {
      std::cerr << "error: JamSpell model training failed\n";
      return 1;
    }
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
