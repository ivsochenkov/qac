{
  description = "qac: query autocomplete library and REST server (hat-trie index, Boost.Beast, optional JamSpell correction)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-24.11";
    flake-utils.url = "github:numtide/flake-utils";

    # The dependencies live in git submodules (see .gitmodules). They are pinned here
    # to the same commits, so that a build does not depend on how the source was
    # obtained (git checkout, tarball, flake reference). When a submodule is updated,
    # update the rev here too and run `nix flake lock`.
    hat-trie = {
      url = "github:ivsochenkov/hat-trie/f2eb2df7ed781a1b51e1646147526f9995c11e42";
      flake = false;
    };
    jamspell = {
      url = "github:ivsochenkov/JamSpell/6d3e49c5b73fbf253df4c2b4ed9364be532c7aad";
      flake = false;
    };
    cereal = {
      url = "github:USCiLab/cereal/22a1b369f39be918ca79206a83c4facd759f9105";
      flake = false;
    };
    googletest = {
      url = "github:google/googletest/a46a6012a133c7c0929fdae55b321c2fe65f3c5f";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, hat-trie, jamspell, cereal, googletest }:
    let
      thirdParty = { inherit hat-trie jamspell cereal googletest; };
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
            # Without JamSpell: no ru_RU locale, no model training in the tests.
            qac-no-jamspell = mkQac { withJamSpell = false; };
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
              if [ ! -e third_party/hat-trie/include ]; then
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
