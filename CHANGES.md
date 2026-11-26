# Changes (2): JamSpell master, always built, third-party code from JamSpell

## What changed

* **JamSpell `master`.** `third_party/jamspell` is the `master` branch of
  <https://github.com/ivsochenkov/JamSpell> (`.gitmodules`: `branch = master`; developed against
  commit `90b9483`). qac's wrapper (`jamspell_fixer.cpp`) and the test helper compile and pass
  against it unchanged.
* **Not optional.** The CMake option `QAC_WITH_JAMSPELL`, the `QAC_HAVE_JAMSPELL` `#ifdef`s and the
  Nix output `qac-no-jamspell` are gone; JamSpell is always built and linked, and its tests always
  run. `--jamspell-model` stays optional at run time: without a model `correct=1` answers `501`
  (`"spell correction is not configured"`), as before.
* **Third-party code comes from JamSpell.** The submodules `hat-trie`, `cereal` and `googletest` of
  qac are removed. hat-trie, array-hash, cereal and robin-map are taken from the submodules nested in
  `third_party/jamspell`, so JamSpell and qac are built from the same copy of each:

  ```
  third_party/jamspell
    third_party/hat-trie
      third_party/array-hash
        third_party/cereal
    third_party/robin-map
  ```

  CMake stops with the name of the missing file if a submodule is not checked out. This also made
  the old workaround unnecessary (JamSpell's targets were built with `tsl` renamed, because the
  old fork carried its own copy of those containers); it is removed.
* **googletest** is an installed package (`find_package(GTest)`, like the fork's own tests):
  `libgtest-dev` on Debian/Ubuntu, `gtest` in Nix.
* **The fork's build bug.** JamSpell `master` does not build from a fresh checkout: its
  `jamspell/CMakeLists.txt` adds `third_party/hat-trie/third_party/include` for array-hash, which
  does not exist (`tsl/array_map.h: No such file or directory`). `cmake/jamspell/CMakeLists.txt`
  works around it, so qac builds with the fork untouched. The fix for the fork is
  `docs/jamspell-fork-array-hash-include.patch` (`git apply` it in the fork; once it is in, the
  workaround only lists the same directory twice).
* **Test model training needs 250 MB instead of 4 GB.** The fork reserves memory for 100 million
  n-grams and a 5 million word dictionary however small the text is. The test build now sets the
  fork's own `SPLL_MAX_GRAMS_SIZE` / `SPLL_MAX_DICT_SIZE` (4 M / 200 k; the test text has 155 k
  n-grams), which are capacities and caps far above the data.
* **Nix** (`flake.nix`, `nix/package.nix`): the four inputs are replaced by one,
  `git+https://github.com/ivsochenkov/JamSpell?ref=master&submodules=1`; `gtest` comes from nixpkgs.
  **Not verified** (no Nix here); the repository has no `flake.lock`, run `nix flake lock`.

## Things to know

1. **hat-trie moves** from `f2eb2df` to the revision JamSpell `master` pins (`c56413d`; array-hash is
   now its own submodule under it). qac's index code compiled unchanged. Snapshots are compatible
   both ways: a snapshot written by a build with the old hat-trie loads in one with the new, and the
   other way round, with identical answers (checked with a driver built against both). The bytes of
   two snapshots of the same data are not identical, however, not even between two runs of the same
   binary (about 0.4% of the bytes differ; probably padding), so do not compare snapshot files
   byte for byte.
2. **Building now needs** the JamSpell submodule tree, Boost `locale`, an installed GoogleTest, and
   the `ru_RU.UTF-8` locale for the tests (they train a model).
3. **To follow the fork's master:**
   `git submodule update --init --remote third_party/jamspell && git submodule update --init --recursive`,
   then commit the new submodule revision.
4. Clang was not tried against the current master (see README, *Compilers*).

## Applying it to your repository

Rehearsed in a scratch repository with the old four-submodule layout (the order matters: `git rm`
refuses to run while `.gitmodules` has unstaged changes):

```sh
git apply qac-jamspell-master.patch     # may warn about whitespace inside the embedded fork patch
git add .gitmodules
git rm -f third_party/hat-trie third_party/cereal third_party/googletest
                                        # "Could not find section in .gitmodules" warnings are expected
rm -rf .git/modules/third_party/{hat-trie,cereal,googletest}      # optional clean-up
git submodule update --init --remote third_party/jamspell         # jamspell -> master
git submodule update --init --recursive
git add -A && git commit
```

The result is `third_party/jamspell` at `90b9483` with hat-trie `c56413d`, array-hash `47caff3`,
cereal `22a1b36` and robin-map `91362aa` nested in it, which is what this change was built and
tested against.

## Files (this change)

Changed: `CMakeLists.txt`, `cmake/jamspell/CMakeLists.txt`, `tests/CMakeLists.txt`, `.gitmodules`,
`src/main.cpp`, `include/qac/jamspell_fixer.hpp`, `flake.nix`, `nix/package.nix`, `README.md`,
`CHANGES.md`. New: `docs/jamspell-fork-array-hash-include.patch`.

---

# Changes (1): tail fallback, long inputs, hot-prefix table

## New behaviour

* **Tail fallback** (`tail`, `min_tail` request parameters; `qac serve --tail --min-tail` for the
  defaults; `qac complete --tail --min-tail`). A lookup that finds fewer than N global / M user
  suggestions is retried with the last `tail` words of the input (the word being typed counts),
  then one word fewer, down to `min_tail`. Defaults: 3 and 2. `tail=0` restores the old behaviour.
  Both lists fall back, independently. Details: README, *Long inputs: the tail fallback*.
* **`matched_words`** on every suggestion: how many trailing words of the input it matches.
* **Over-long input** to `/v1/complete` (more than 32 words or 1024 bytes) is no longer a `400`:
  only its end is used, and `input_truncated: true` is returned. Storing queries stays strict.
* **`correct=1`** corrects only the words a lookup can use (the last `tail` words). With
  `tail=0` it corrects all complete words as before.
* **Hot-prefix table** (`--hot-prefix-letters 2`, `--hot-prefix-top 100`): exact, precomputed top
  lists for a partial first word of 1-2 letters, kept current by writes. Details: README,
  *Hot-prefix table*. `/v1/stats` gains `hot_lookups`, `hot_prefixes`, `hot_entries`.
* **Dictionary scan skipped** when the complete words in front of a partial word start no stored
  query (one trie probe; same answers, much cheaper for the fallback's empty lookups).
* Demo page: `tail words` / `down to` fields; accepting a suggestion that matched only the last
  words replaces just those words, and shows a "last N words" label.
* `qac_bench`: `--hot-prefix-letters`, `--tail`, `--min-tail`, `--junk-words`, `--short-partials`;
  prints the share of lookups with no result.

## Things that behave differently from 0.1.0

1. **The fallback is on by default** (tail 3, min_tail 2): `/v1/complete` can now return
   suggestions for inputs that used to return nothing, and lists more than the plain lookup
   would for inputs with fewer than N matches. Start the server with `--tail 0` to keep the
   old behaviour server-wide.
2. A word the index has never seen no longer empties the result; it only disables the lookups
   that contain it.
3. The JSON of a suggestion has a new field (`matched_words`).
4. The server builds the hot-prefix table at start-up and after `optimize` (about 0.1 µs per
   query). `--hot-prefix-letters 0` switches it off. For 1-2 letter first words the answer is now
   exact where it used to be approximate (and `truncated` is false).
5. `Index::ScanStats` has two new fields (`scanned`, `hot`); `Index` has
   `enable_hot_prefixes()`; `TextProcessor` has `parse_typed()` (`parse_prefix` is unchanged).

## Files

Changed: `include/qac/{index,engine,api,text}.hpp`, `src/{index,engine,api,text,main}.cpp`,
`web/index.html`, `bench/bench.cpp`, `README.md`, `tests/{CMakeLists.txt,test_text.cpp,test_api.cpp}`.
New: `tests/test_hot.cpp`, `tests/test_tail.cpp`, `docs/PROPOSALS.md`, this file.
