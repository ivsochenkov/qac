# Proposals

What came out of reading the code and measuring it, and was **not** implemented in this change
(the tail fallback, the handling of long inputs and the hot-prefix table are described in
`README.md` and `CHANGES.md`). Ordered by how much I expect each to matter; the effort is a
rough guess. Numbers are from the benchmark in `README.md` unless said otherwise.

## 1. Match the tail anywhere in a stored query, not only at its start

**Problem.** Everything in the index, the tail fallback included, matches queries that *start
with* the typed words. `foxes ju` finds `foxes juggernaut` but not `the quick foxes jump`. For
free-text queries that is often the more natural match.

**Idea.** Also index every *word suffix* of a stored query: for `the quick foxes jump` the keys
`quick foxes jump` and `foxes jump` (and `jump`), each pointing at the same query id and counted
at a lower weight (for example frequency / 4, so exact-start matches still win). The existing
trie, scan and ranking code can be reused as it is; the fallback tiers would search the suffix
keys after the start-of-query keys.

**Cost.** Memory and training time grow by the mean number of words per query (about 3.5 in the
benchmark, so roughly 3-4x for the query trie). That is the real decision: if the pool is
hundreds of millions of queries it may not fit, and an inverted index (word id -> queries
containing it, intersected for the tail words) is the alternative with a different trade-off:
less memory, slower lookups. Worth a prototype on real data before choosing. Effort: large.

## 2. A trigger threshold for the fallback

The fallback starts whenever a lookup finds *fewer than N* results, as asked. On the normal mix
that costs about 7% throughput (measured), but on a deployment with a large N it will run for
most long inputs. A request parameter such as `tail_below` (run the fallback only if fewer than
this many were found; default N, 1 means "only when nothing was found") would let a client trade
quality for latency per request. Effort: small (one more number in the tier loop and the API).

## 3. Make the lookups after a complete word exact and cheaper

A partial word after complete words (`new yo`) is resolved through the dictionary: the 64 most
frequent words starting with `yo` *in the whole pool* are tried under the prefix. A word that is
rare overall but common after `new` can be missed, so the answer is approximate even when no
budget runs out. When the prefix has few entries it is better to scan the prefix's subtree once
and keep the entries whose next word starts with the typed letters; `top_user` already works that
way. Choosing by the size of the prefix's subtree (one `equal_prefix_range` walk with a cap) gives
exact answers for small subtrees and keeps the dictionary path for big ones. This is also what
the tail fallback's shorter lookups would benefit from most. Effort: medium.

## 3b. Extend the hot-prefix table to the other expensive shapes

The README's own breakdown after the 1-2 letter shapes: one complete first word (13% of the
time), 3-letter partial first words (9%), 4+ letters (20%). The same "exact top list that writes
keep current" construction works for any frequent prefix of the key, not only for letters of the
first word: for example the top-K lists of the most frequent first words (`weather`, `how to`).
The table would then be keyed by word ids, bounded by a "top M head words" setting, and the
maintenance rule in `Index::hot_upsert` / `hot_erase` already handles the invariants. Effort:
medium; the benefit depends on how skewed real traffic is.

## 4. `rank=avg_word_freq` for the hot lists

The hot-prefix table serves only `rank=frequency`, because a list ordered by frequency cannot
answer the other ordering exactly. If that ranking is used for short prefixes in production, keep
a second list per prefix ordered by it. The mean word frequency of a query changes whenever *any*
of its words' weights change, so this list cannot be maintained incrementally the way the
frequency list is; it would need periodic rebuilds and be approximate between them. Probably
only worth it if the ranking turns out to be popular. Effort: medium.

## 5. Typo tolerance for the word being typed

`correct=1` fixes only complete words (JamSpell needs context and a finished word). The word
being typed (`red foxs ju`) is matched exactly as a prefix. A bounded edit-distance-1 search among
the dictionary candidates (deletion/transposition/substitution of one letter, only for words of
three or more letters) would recover most slips while typing. It multiplies the dictionary work,
so it should only run when the exact lookup found fewer than N results, like the fallback.
Effort: medium.

## 6. Tell the client where the match starts

`matched_words` makes a client count words the way the server does (whitespace plus the
alphabet's `@separators`); the demo page has a copy of that list. A `matched_from` field (offset
in code points into `q`) would make the replacement a plain `slice`. The tokenizer would have to
keep token positions, which it does not do today (`parse_typed` re-tokenizes a `u32string`).
Effort: small to medium; mainly worthwhile if there are several client implementations.

## 7. Observability of the fallback

`/v1/stats` has `hot_lookups`; it does not say how often the fallback ran or how deep it went.
Counters for "requests that used a shorter lookup", per depth, and "requests that still returned
nothing" would show whether `tail` / `min_tail` are well chosen for real traffic and whether
proposal 1 is needed. Effort: small.

## 8. Alternative merge across lookups

Results are listed by specificity: every match of the longer lookup precedes any match of a
shorter one, even a far more frequent one. That is predictable and what was asked for, but a
score such as `freq * w(matched_words)` (or interleaving) might rank better on real data. It is
a one-function change in `Engine::complete` once there is a way to evaluate rankings (click logs).
