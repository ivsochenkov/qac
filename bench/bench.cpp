// Benchmark on synthetic data: bulk training, then prefix lookups from 1..N threads.
//
//   qac_bench [--queries 1000000] [--vocab 200000] [--lookups 200000] [--threads 1,2,4] [--cache N] [--rank frequency|avg_word_freq] [--dir DIR]
//
// Words are pseudo-random strings with Zipf-distributed popularity, queries have 1-6
// words and Zipf-distributed counts, so the shape resembles real query logs (a few
// very frequent words and a long tail). The numbers depend entirely on that shape;
// use real data for capacity planning.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>

#include "qac/engine.hpp"
#include "qac/log.hpp"
#include "qac/train.hpp"

namespace po = boost::program_options;
namespace fs = std::filesystem;
using namespace qac;
using Clock = std::chrono::steady_clock;

namespace {

double seconds(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double>(b - a).count();
}

std::uint64_t rss_kib() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("VmRSS:", 0) == 0) return std::strtoull(line.c_str() + 6, nullptr, 10);
  return 0;
}

// Zipf sampler over [0, n) by inverse CDF on precomputed weights.
class Zipf {
 public:
  Zipf(std::size_t n, double s) : cdf_(n) {
    double sum = 0;
    for (std::size_t i = 0; i < n; ++i) cdf_[i] = (sum += 1.0 / std::pow(static_cast<double>(i + 1), s));
    for (auto& c : cdf_) c /= sum;
  }
  template <class Rng>
  std::size_t operator()(Rng& rng) const {
    const double u = std::uniform_real_distribution<double>(0, 1)(rng);
    return static_cast<std::size_t>(std::lower_bound(cdf_.begin(), cdf_.end(), u) - cdf_.begin());
  }

 private:
  std::vector<double> cdf_;
};

std::string pseudo_word(std::mt19937_64& rng) {
  static const char* kSyll[] = {"ba", "be", "bi", "bo", "ka", "ke", "ki", "ko", "la", "le", "li", "lo",
                                "ma", "me", "mi", "mo", "na", "ne", "ni", "no", "ra", "re", "ri", "ro",
                                "sa", "se", "si", "so", "ta", "te", "ti", "to", "st", "tr", "pl", "ch"};
  const int n = 1 + static_cast<int>(rng() % 4);
  std::string w;
  for (int i = 0; i < n; ++i) w += kSyll[rng() % (sizeof kSyll / sizeof *kSyll)];
  return w;
}

struct Percentiles {
  double p50, p95, p99, max;
};
Percentiles percentiles(std::vector<double>& v) {
  std::sort(v.begin(), v.end());
  auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; };
  return {at(0.50), at(0.95), at(0.99), v.back()};
}

}  // namespace

int main(int argc, char** argv) {
  std::size_t queries = 1'000'000, vocab = 200'000, lookups = 200'000, cache = 0;
  std::string rank = "frequency";
  std::string threads_arg = "1,2,4", dir, alphabet_path = QAC_BENCH_ALPHABET;
  po::options_description d("Options");
  d.add_options()("queries", po::value(&queries)->default_value(queries), "distinct queries to generate")
      ("vocab", po::value(&vocab)->default_value(vocab), "vocabulary size")
      ("lookups", po::value(&lookups)->default_value(lookups), "lookups per thread-count run")
      ("threads", po::value(&threads_arg)->default_value(threads_arg), "comma separated thread counts")
      ("rank", po::value(&rank)->default_value(rank), "frequency | avg_word_freq")
      ("cache", po::value(&cache)->default_value(cache), "result cache entries (0: off, measures the index itself)")
      ("dir", po::value(&dir), "working directory (default: a temporary one)")
      ("alphabet", po::value(&alphabet_path)->default_value(alphabet_path), "alphabet file")
      ("help,h", "this help");
  po::variables_map vm;
  try {
    po::store(po::parse_command_line(argc, argv, d), vm);
    po::notify(vm);
  } catch (const po::error& e) {
    std::cerr << e.what() << "\n";
    return 2;
  }
  if (vm.count("help")) return std::cout << d << "\n", 0;
  set_log_level(LogLevel::Warn);

  std::vector<unsigned> thread_counts;
  {
    std::stringstream ss(threads_arg);
    std::string t;
    while (std::getline(ss, t, ',')) thread_counts.push_back(static_cast<unsigned>(std::stoul(t)));
  }
  const bool own_dir = dir.empty();
  if (own_dir) dir = (fs::temp_directory_path() / ("qac-bench-" + std::to_string(::getpid()))).string();
  fs::create_directories(dir);

  // ---- generate ----
  std::mt19937_64 rng(12345);
  std::vector<std::string> words(vocab);
  for (auto& w : words) w = pseudo_word(rng) + std::to_string(rng() % 7);  // keeps most distinct
  const Zipf word_zipf(vocab, 1.05);
  const std::string input = (fs::path(dir) / "queries.txt").string();
  std::vector<std::string> sample_queries;  // for lookups
  {
    std::ofstream out(input);
    const Zipf count_zipf(10000, 1.2);
    for (std::size_t i = 0; i < queries; ++i) {
      const int nw = 1 + static_cast<int>(rng() % 6);
      std::string q;
      for (int k = 0; k < nw; ++k) q += (k ? " " : "") + words[word_zipf(rng)];
      out << q << '\t' << (count_zipf(rng) + 1) << '\n';
      if (i % 50 == 0) sample_queries.push_back(q);
    }
  }
  std::printf("generated %zu queries over a vocabulary of %zu words\n", queries, vocab);

  const Alphabet alphabet = Alphabet::load(alphabet_path);

  // ---- train ----
  const std::string data = (fs::path(dir) / "data").string();
  TrainOptions to;
  to.input = input;
  to.data_dir = data;
  to.overwrite = true;
  to.progress_every = 0;
  const auto t0 = Clock::now();
  const TrainReport rep = train(alphabet, to);
  const double train_s = seconds(t0, Clock::now());
  std::printf("train:    %.2f s, %.0f queries/s, %llu distinct queries, %llu words, snapshot %.1f MiB\n", train_s,
              static_cast<double>(rep.accepted) / train_s, static_cast<unsigned long long>(rep.index.global_queries),
              static_cast<unsigned long long>(rep.index.words), static_cast<double>(rep.snapshot_bytes) / 1048576.0);

  // ---- load ----
  EngineConfig ec;
  ec.data_dir = data;
  ec.fsync = FsyncPolicy::Never;
  ec.snapshot_interval_s = 0;
  ec.snapshot_wal_bytes = 0;
  ec.cache_entries = cache;  // off by default: measure the index, not the cache
  const auto t1 = Clock::now();
  Engine engine(alphabet, ec);
  std::printf("load:     %.2f s, resident %.1f MiB, tables %.1f MiB\n", seconds(t1, Clock::now()),
              static_cast<double>(rss_kib()) / 1024.0, static_cast<double>(engine.stats().index.arena_bytes) / 1048576.0);

  // ---- lookups: a mix of complete-word prefixes and partial last words ----
  auto make_prefix = [&](std::mt19937_64& r) {
    const std::string& q = sample_queries[r() % sample_queries.size()];
    // cut at a random position: sometimes at a word boundary, mostly mid-word
    std::size_t cut = 1 + r() % q.size();
    if (r() % 3 == 0) {
      const auto sp = q.find(' ', cut);
      cut = sp == std::string::npos ? q.size() : sp + 1;
    }
    return q.substr(0, cut);
  };

  std::printf("\n%-8s %12s %10s %10s %10s %10s %10s\n", "threads", "lookups/s", "p50 us", "p95 us", "p99 us", "max us",
              "truncated");
  for (unsigned threads : thread_counts) {
    std::vector<std::vector<double>> lat(threads);
    std::atomic<std::uint64_t> truncated{0};
    const std::size_t per_thread = lookups / threads;
    const auto start = Clock::now();
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t)
      pool.emplace_back([&, t] {
        std::mt19937_64 r(1000 + t);
        lat[t].reserve(per_thread);
        for (std::size_t i = 0; i < per_thread; ++i) {
          CompleteRequest req;
          req.query = make_prefix(r);
          req.n = 10;
          if (rank == "avg_word_freq") req.rank = Index::GlobalRank::AverageWordFrequency;
          const auto a = Clock::now();
          const CompleteResult res = engine.complete(req);
          lat[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
          if (res.truncated) ++truncated;
        }
      });
    for (auto& th : pool) th.join();
    const double wall = seconds(start, Clock::now());
    std::vector<double> all;
    for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
    const Percentiles p = percentiles(all);
    std::printf("%-8u %12.0f %10.1f %10.1f %10.1f %10.1f %9.2f%%\n", threads,
                static_cast<double>(all.size()) / wall, p.p50, p.p95, p.p99, p.max,
                100.0 * static_cast<double>(truncated.load()) / static_cast<double>(all.size()));
  }

  // ---- writes ----
  {
    EngineConfig wc = ec;
    const auto w0 = Clock::now();
    constexpr int kWrites = 100000;
    for (int i = 0; i < kWrites; ++i)
      engine.add_user(static_cast<std::uint32_t>(i % 1000), words[i % 100] + " " + words[(i * 7) % 5000], 1);
    const double s = seconds(w0, Clock::now());
    std::printf("\nuser writes (WAL, fsync=never): %.0f/s\n", kWrites / s);
  }

  if (own_dir) fs::remove_all(dir);
  return 0;
}
