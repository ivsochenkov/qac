# Changes: tail fallback, long inputs, hot-prefix table

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
