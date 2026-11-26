{ lib
, stdenv
, cmake
, ninja
, boost
, makeWrapper
, glibcLocales
, gtest
  # The source tree and the pinned third-party sources (a flake input: JamSpell with its
  # nested submodules).
, src
, thirdParty
  # Build and run the test-suite. The tests train a small JamSpell model (about 250 MB of
  # RAM) and need the ru_RU.UTF-8 locale (see LOCALE_ARCHIVE below).
, doCheck ? true
}:

stdenv.mkDerivation {
  pname = "qac";
  version = "0.1.0";

  inherit src doCheck;

  nativeBuildInputs = [ cmake ninja makeWrapper ];
  buildInputs = [ boost ] ++ lib.optional doCheck gtest;
  nativeCheckInputs = [ glibcLocales ];

  # The JamSpell fork constructs std::locale("ru_RU.UTF-8"); glibc finds it through
  # LOCALE_ARCHIVE. Needed while building (the tests train a model) and when running.
  LOCALE_ARCHIVE = "${glibcLocales}/lib/locale/locale-archive";

  # Replace whatever the source contains in third_party (empty submodule directories, or a
  # checkout) with the pinned input. It brings hat-trie, array-hash, cereal and robin-map in
  # its own third_party directory, which is where the build looks for them.
  postPatch = ''
    rm -rf third_party
    mkdir -p third_party
    cp -r --no-preserve=mode ${thirdParty.jamspell} third_party/jamspell
  '';

  cmakeFlags = [
    (lib.cmakeFeature "CMAKE_BUILD_TYPE" "Release")
    (lib.cmakeBool "QAC_BUILD_TESTS" doCheck)
    (lib.cmakeBool "QAC_BUILD_BENCH" true)
  ];

  postInstall = ''
    wrapProgram $out/bin/qac \
      --set-default LOCALE_ARCHIVE ${glibcLocales}/lib/locale/locale-archive
  '';

  meta = {
    description = "Query autocomplete: hat-trie index, snapshot + WAL persistence, REST server";
    mainProgram = "qac";
    platforms = lib.platforms.linux;
  };
}
