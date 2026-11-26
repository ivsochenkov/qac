{
  description = "qac: query autocomplete library and REST server (hat-trie index, Boost.Beast, JamSpell correction)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";

    # All third-party code comes from the JamSpell fork (branch master) and the submodules nested
    # in it: hat-trie (with its array-hash and cereal) and robin-map; see .gitmodules and
    # CMakeLists.txt. A github: input would not contain submodules, hence git+https with
    # submodules=1. `nix flake lock` pins the commit in flake.lock; `nix flake update jamspell`
    # moves it to the current master. (googletest is the gtest package of nixpkgs.)
    jamspell = {
      url = "git+https://github.com/ivsochenkov/JamSpell?ref=master&submodules=1";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, jamspell }:
    let
      thirdParty = { inherit jamspell; };
    in
    flake-utils.lib.eachDefaultSystem
      (system:
        let
          pkgs = import nixpkgs { inherit system; };
          mkQac = args: pkgs.callPackage ./nix/package.nix ({ src = self; inherit thirdParty; } // args);
        in
        {
          packages = rec {
            # Server and CLI with spell correction (JamSpell fork).
            qac = mkQac { };
            # The same sources built with Clang.
            qac-clang = mkQac { stdenv = pkgs.clangStdenv; };
            default = qac;
          };

          apps.default = {
            type = "app";
            program = "${self.packages.${system}.qac}/bin/qac";
          };

          devShells.default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.qac ];
            packages = with pkgs; [ gdb clang-tools valgrind ];
            # The JamSpell fork needs the ru_RU.UTF-8 locale (see README).
            LOCALE_ARCHIVE = "${pkgs.glibcLocales}/lib/locale/locale-archive";
            shellHook = ''
              if [ ! -e third_party/jamspell/third_party/robin-map/include ]; then
                echo "qac: third_party is empty. Run: git submodule update --init --recursive"
              fi
              echo "qac dev shell: cmake -S . -B build -G Ninja && cmake --build build && ctest --test-dir build"
            '';
          };

          checks.build = self.packages.${system}.qac;

          formatter = pkgs.nixpkgs-fmt;
        })
    // {
      overlays.default = final: prev: {
        qac = final.callPackage ./nix/package.nix { src = self; inherit thirdParty; };
      };

      nixosModules.default = import ./nix/module.nix self;
    };
}
