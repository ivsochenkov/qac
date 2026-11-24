{ lib
, stdenv
, cmake
, ninja
, boost
, makeWrapper
, glibcLocales
  # The source tree and the pinned third-party sources (flake inputs).
, src
, thirdParty
  # Spell correction through the JamSpell fork.
, withJamSpell ? true
  # Build and run the test-suite. With JamSpell the tests train a small model, which
  # needs a few GB of RAM.
, doCheck ? true
}:

stdenv.mkDerivation {
  pname = "qac";
  version = "0.1.0";

  inherit src doCheck;

  nativeBuildInputs = [ cmake ninja makeWrapper ];
  buildInputs = [ boost ];
  nativeCheckInputs = [ glibcLocales ];

  # The JamSpell fork constructs std::locale("ru_RU.UTF-8"); glibc finds it through
  # LOCALE_ARCHIVE. Needed while building (the tests train a model) and when running.
  LOCALE_ARCHIVE = "${glibcLocales}/lib/locale/locale-archive";

  # Replace whatever the source contains in third_party (empty submodule directories,
  # or a checkout) with the pinned inputs.
  postPatch = ''
    rm -rf third_party
    mkdir -p third_party
    cp -r --no-preserve=mode ${thirdParty.hat-trie} third_party/hat-trie
    cp -r --no-preserve=mode ${thirdParty.cereal} third_party/cereal
    ${lib.optionalString withJamSpell "cp -r --no-preserve=mode ${thirdParty.jamspell} third_party/jamspell"}
    ${lib.optionalString doCheck "cp -r --no-preserve=mode ${thirdParty.googletest} third_party/googletest"}
  '';

  cmakeFlags = [
    (lib.cmakeFeature "CMAKE_BUILD_TYPE" "Release")
    (lib.cmakeBool "QAC_WITH_JAMSPELL" withJamSpell)
    (lib.cmakeBool "QAC_BUILD_TESTS" doCheck)
    (lib.cmakeBool "QAC_BUILD_BENCH" true)
  ];

  postInstall = lib.optionalString withJamSpell ''
    wrapProgram $out/bin/qac \
      --set-default LOCALE_ARCHIVE ${glibcLocales}/lib/locale/locale-archive
  '';

  meta = {
    description = "Query autocomplete: hat-trie index, snapshot + WAL persistence, REST server";
    mainProgram = "qac";
    platforms = lib.platforms.linux;
  };
}
