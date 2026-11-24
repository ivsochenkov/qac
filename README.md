# qac — query autocomplete

![qac!](qac.png)

A C++17 library and REST server that completes what a user is typing from a pool of
past queries: the **global** pool (what everybody searches) and the **user's own
history**, ranked by frequency. Built for hundreds of millions of queries, UTF-8 text,
and updates while the server runs.

* Dictionary and query index on a **hat-trie** (`third_party/hat-trie`, your fork).
  Every word is a `std::uint32_t` id; a query is a sequence of ids.
* **Boost.Tokenizer** segments the input; a **custom alphabet** (≤255 letters, one byte
  per letter, case/accent folding) normalises it.
* **Boost.Asio / Beast** HTTP server: I/O threads plus a separate worker `thread_pool`.
* **Snapshot + write-ahead log** persistence with crash recovery.
* Optional **spell correction** through your JamSpell fork (`FixFragment`), off unless
  a request asks for it.
* CMake build, git submodules, `flake.nix`. C++17, Boost ≥ 1.75.

## Build

```sh
git submodule update --init --recursive     # hat-trie, JamSpell fork, cereal, googletest
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build -j2
```

| option | default | |
|---|---|---|
| `QAC_WITH_JAMSPELL` | ON | spell correction through `third_party/jamspell` |
| `QAC_BUILD_TESTS` | ON | GoogleTest suite |
| `QAC_BUILD_BENCH` | ON | `qac_bench` |
| `QAC_SANITIZE` | OFF | AddressSanitizer + UBSan |

Dependencies besides the submodules: Boost ≥ 1.75 (`program_options`, `json`, and
`locale` for JamSpell; Asio, Beast, Tokenizer, Interprocess are header-only).

**Compilers.** GCC 13 builds everything. Clang 18 builds and passes the whole suite with
`-DQAC_WITH_JAMSPELL=OFF`; with JamSpell ON it fails *inside the JamSpell fork*
(`spell_corrector.cpp`, `lang_model.cpp`: a `boost::container::string` is passed where
`const std::string&` is expected, which libstdc++ tolerates and Clang rejects). The fork is
treated as an untouched external dependency, so this needs a fix there. Until then use
GCC for builds that include JamSpell.

**JamSpell needs the `ru_RU.UTF-8` locale** on the machine that loads the model (the fork
constructs `std::locale("ru_RU.UTF-8")`). Debian/Ubuntu: `locale-gen ru_RU.UTF-8`. Nix: the
flake sets `LOCALE_ARCHIVE` for you. `qac` reports this with an actionable message.

### Nix

```sh
nix build .                      # qac with JamSpell (also: .#qac-no-jamspell, .#qac-clang)
nix run . -- serve --data-dir ./data
nix develop                      # dev shell with the toolchain and LOCALE_ARCHIVE
```

The four dependencies are pinned as flake inputs at the commits of the git submodules
(`flake.nix`; keep them in sync when you update a submodule). A NixOS module is provided
(`nixosModules.default`, `services.qac.enable = true;`).

> **Not verified:** the Nix files could not be built in the environment they were written
> in (no access to nixpkgs). They parse, and the package/module expressions were evaluated
> against stand-ins, but the first real `nix build` may need small fixes.

## Quick start

```sh
./build/qac train data/sample_queries.txt --data-dir ./qac-data
./build/qac serve --data-dir ./qac-data --port 8080
# open http://localhost:8080/ for the demo page
curl 'localhost:8080/v1/complete?q=weather+n'
```

`qac` looks for the alphabet in `--alphabet`, `$QAC_ALPHABET`, `<prefix>/share/qac/alphabet.txt`
and finally the bundled `data/alphabet.txt`.

## Commands

| command | |
|---|---|
| `qac train` | build a data directory from a file, one query per line (`query` or `query<TAB>count`) |
| `qac serve` | run the REST server |
| `qac complete` | look a query up from the command line (server must be stopped) |
| `qac inspect` | show what a data directory contains |
| `qac optimize` | compact a data directory offline |

`qac <command> --help` lists every option (Boost.Program_options).

### Train mode

Reads the file line by line and writes a snapshot. It deliberately bypasses the WAL: the
index is built in memory and written once (≈400k queries/s on one core in the benchmark).
Malformed lines (characters outside the alphabet, too long, bad UTF-8, a zero or oversized
count) are skipped and reported with their line numbers. An existing data directory is only
replaced with `--overwrite`. A trailing `<TAB><digits>` is the count; use `--no-counts` if
tabs followed by digits are part of your queries.

## REST API

| | |
|---|---|
| `GET /v1/complete?q=<text>[&n=10][&user=<id>&m=5][&correct=1][&rank=avg_word_freq]` | top-N global + top-M from the user's history |
| `POST /v1/queries` `{"query":"…","count":1}` | add to the global pool |
| `POST /v1/users/{user}/queries` `{"query":"…","count":1,"also_global":false}` | add to a user's history |
| `DELETE /v1/queries/{id}` | remove from the global pool |
| `DELETE /v1/users/{user}/queries/{id}` | remove from one user's history |
| `POST /v1/snapshot` | persist now and truncate the WAL |
| `POST /v1/optimize` | compact dictionary and tables, replace the snapshot |
| `GET /v1/stats`, `GET /healthz`, `GET /` | statistics, liveness, demo page |

`user` and query ids are `uint32`. A query id is stable and never reused. `count` defaults to 1.

```
$ curl 'localhost:8080/v1/complete?q=new+york+&n=2&user=42&m=2'
{"global":[{"id":7,"text":"new york times","freq":720},{"id":8,"text":"new york pizza","freq":380}],
 "user":[{"id":131,"text":"new york marathon","freq":3,"last_used":1795118145}],
 "truncated":false,"took_us":18}
```

* `q` is required (it may be empty: then only the user's history is returned).
  The last word is treated as *being typed* unless the text ends with a separator.
* `correct=1` spell-corrects the **complete** words (never the one being typed) and
  reports the corrected text in `corrected`. It is off by default and answers `501` when
  the server has no model.
* Whole queries are ranked by frequency (ties: older id first; for users: more recent first).
* `rank=avg_word_freq` re-ranks the **global** candidates (the top-N) by the *mean frequency of
  the words each candidate consists of*; `rank=frequency` (the default) is the plain order above.
  See *Ranking by mean word frequency* below. The user's list is not affected.
* `truncated: true` means a scan budget ran out (see *Limits*), so the list may be approximate.
* Errors are JSON `{"error":"bad_request","message":"…"}` with `400` (malformed input,
  including characters outside the alphabet), `404`, `405`, `409` (maintenance already
  running), `413`, `501`.
* Malformed queries are **rejected whole** on every endpoint; nothing is partially stored.

## Ranking by mean word frequency

Request parameter `rank=avg_word_freq` (CLI: `qac complete --rank avg_word_freq`, demo page:
the "global ranking" selector). For every candidate of the global pool the **mean of the
frequencies of its words** is computed, and the N candidates with the highest mean are returned.

* The frequency of a *word* is the number of times it occurs in the global pool, counting each
  query as many times as its own frequency, and each occurrence inside a query (a word that
  appears twice in a query counts twice). It follows every add and remove, and is the same
  number the engine already keeps to pick dictionary words for a partial last word.
* All words of the candidate count, including the ones already typed.
* Example: `apple`×100, `apple pie`×10, `apple juice`×1 and `juice`×1000. The words have the
  frequencies apple 112, pie 10, juice 1001, so for `apple ` the means are `apple juice` 556.5,
  `apple` 112, `apple pie` 61; by query frequency `apple juice` would come last.
* Ties are broken by the query's own frequency, then by the older id. Means are compared as exact
  fractions, not floating point, so the order is deterministic.
* Each global suggestion then carries `"score"`: the mean (absent in the default mode).
  `freq` is still the query's own frequency.
* It scans the same entries and obeys the same budgets as the default ranking (`truncated` is
  unchanged) but costs more per entry, see *Limits*. The result cache keeps the two rankings apart.

## Alphabet

`data/alphabet.txt` (own format, not JamSpell's):

```
# a letter class: all characters are the same letter; the first is the canonical form
a A
е Е ё Ё
ß ẞ
@separators . , ; : ! ? ( ) " - _ …
```

Up to 255 classes (one byte each, code 0 reserved), `U+XXXX` notation allowed.
Whitespace always separates words. A character that is neither a letter nor a separator
makes the query malformed. The alphabet's fingerprint is stored in snapshots and the WAL,
so data built with one alphabet is refused under another. The language-agnostic default covers
English, Russian, German, Spanish and French letters and digits.

## Persistence and operation

* **One owner per data directory** (`LOCK`, a kernel file lock that dies with the process).
* Every write is appended to the **WAL** (`wal-*.log`, CRC-protected records, fsync policy
  `always | interval (100 ms) | never`) *before* it is applied. After a crash the snapshot is
  loaded and the WAL replayed; a torn tail record is discarded, damage elsewhere is an error.
* **Snapshots** (`snapshot.bin`, cereal binary + CRC-32C, verified before parsing) are written
  every 15 minutes, when the WAL reaches 1 GiB, on `SIGTERM`/`SIGINT` and on
  `POST /v1/snapshot`; covered WAL segments are then deleted. With `--snapshot-mode fork`
  (default) a forked child serializes a copy-on-write view, so neither readers nor writers
  wait; `inline` stops the world instead.
* **`optimize`** builds a compacted instance (dead queries and words dropped), catches up with
  writes that arrived meanwhile, swaps it in and replaces the snapshot file. It needs roughly
  twice the memory while it runs.
* A small LRU+TTL **cache** serves repeated global lookups (2 s staleness at most;
  removals clear it immediately).
* Each record reaches the operating system (one unbuffered `write`) before the request is
  answered, so a **process crash** (`kill -9`, OOM kill) loses nothing that was acknowledged.
  A **power loss or kernel crash** loses at most the last `--fsync-interval-ms` (100 ms) with
  the default policy, and nothing with `--fsync always`.

> **Deviation from the first brief:** the memory-mapped Boost.Interprocess file with a 1.2
> growth factor and 1 GiB initial size was replaced, at your request, by heap structures with
> snapshot + WAL (`optimize` keeps its role: compact, swap, replace the file).

## Limits and trade-offs

* **Hot and very short prefixes are answered approximately.** Top-N by frequency under a
  prefix means scanning that prefix's subtree; budgets (`--max-visited`,
  `--max-user-visited`, `--max-candidates`) bound the work and set `truncated`. For a partial
  last word only the 64 most frequent matching dictionary words are tried.
  Measured with `qac_bench` (1M synthetic queries, GCC 13, 2 cores, one thread, prefixes cut at
  random positions so 1–2 letter prefixes are over-represented; real logs will differ):

  | | lookups/s | p50 | p95 | p99 | flagged `truncated` |
  |---|---|---|---|---|---|
  | `rank=frequency`, index only (`--cache 0`) | ≈2.3k | 31 µs | 2.9 ms | 5.7 ms | 36% |
  | `rank=avg_word_freq`, index only | ≈0.84k | 44 µs | 8.0 ms | 15.7 ms | 36% |
  | `rank=frequency`, result cache on (`--cache 10000`) | ≈26.7k | 10 µs | 185 µs | 326 µs | 35% |
  | `rank=avg_word_freq`, result cache on | ≈17.6k | 10 µs | 225 µs | 466 µs | 35% |

  Two threads double the throughput (shared read lock). The average ranking is about 2.7× slower
  per uncached lookup because every scanned entry needs its word list and one frequency read per
  word instead of a single stored number.

  **Where the time goes** (callgrind plus a per-shape driver): lookup cost is proportional to the
  number of trie entries scanned, about 80–100 ns each, and nearly all of it is lookups that have
  no complete word before the one being typed:

  | query shape | share of time | mean | entries scanned | flagged `truncated` |
  |---|---|---|---|---|
  | 1-letter partial first word | 40% | 6.9 ms | ≈87,000 | 100% |
  | 2-letter partial first word | 15% | 2.5 ms | ≈31,000 | 100% |
  | 3-letter partial first word | 9% | 1.3 ms | ≈12,800 | 57% |
  | 4+ letter partial first word | 20% | 1.1 ms | ≈11,300 | 14% |
  | one complete first word | 13% | 1.1 ms | ≈11,600 | 0% |
  | anything after a first word | 4% | 6–250 µs | under 11,300 | 0–100% |

  Inside a scan the time splits roughly as: hat-trie bucket iteration 27%, moving between trie
  nodes (binary search in the children map) 25%, top-N selection and hit construction 16%,
  key-prefix comparison 8%; the dictionary scan for a partial word is only about 3–7%. So the fix
  is to scan less, not to tune the scan: a precomputed top-K per frequent first-word prefix
  would turn those lookups into a merge of a few short lists, and also make them exact. The
  result cache already absorbs the repeated head prefixes. The scan cost grows with the data: at
  hundreds of millions of queries a head word holds millions of entries, so the default budget
  (1M entries, tens of ms) will be reached for them.

* Queries are limited to 32 words, 64 letters per word, 1024 bytes (configurable in `Limits`).
* JamSpell is not guaranteed thread-safe by contract; the test-suite checks that concurrent
  `FixFragment` calls are consistent (8 threads), nothing more.
* The server speaks plain HTTP; put TLS and authentication in a reverse proxy. The
  maintenance endpoints can be disabled with `--no-admin`.

## Tests and tools

```sh
ctest --test-dir build -j2        # 200+ tests: index (with brute-force oracles), text, WAL, snapshots, engine (fork and
                                  # inline modes, crash, concurrency), train, REST, HTTP, JamSpell
build/qac_bench --queries 1000000 # synthetic benchmark: training, lookups on 1/2/4 threads
```

The suite also passes under AddressSanitizer + UBSan + LeakSanitizer and ThreadSanitizer.
The JamSpell tests train a small model once at build time (≈4 GB RAM) and skip themselves
if `ru_RU.UTF-8` is missing.

## Layout

```
include/qac, src/     core: alphabet, text, index, wal, snapshot, engine, train, api, http_server
src/main.cpp          the qac command
cmake/jamspell/       build shim for the JamSpell fork (see the comment inside)
web/index.html        demo page, compiled into the binary
data/                 alphabet.txt, sample_queries.txt
tests/, bench/        GoogleTest suite and benchmark
nix/, flake.nix       package, NixOS module, dev shell
third_party/          submodules: hat-trie, jamspell, cereal, googletest
```

The JamSpell fork bundles its own, older copy of the `tsl` containers. To keep it from
clashing with the hat-trie, the fork's targets are built with `tsl` renamed
(`cmake/jamspell/CMakeLists.txt`), and the only qac file that sees the fork's headers
(`src/jamspell_fixer.cpp`) includes nothing else from this project.
