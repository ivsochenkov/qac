// qac: command line front end.
//
//   qac train    --input queries.txt --data-dir DIR    build a data directory from a file
//   qac serve    --data-dir DIR [--port 8080] ...      REST server (one owner per directory)
//   qac complete --data-dir DIR -q "text" ...          one-off lookup (server must be stopped)
//   qac inspect  --data-dir DIR                        print what the directory contains
//   qac optimize --data-dir DIR                        offline compaction (server must be stopped)
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <boost/program_options.hpp>

#include "qac/api.hpp"
#include "qac/engine.hpp"
#include "qac/http_server.hpp"
#include "qac/log.hpp"
#include "qac/train.hpp"
#ifdef QAC_HAVE_JAMSPELL
#include "qac/jamspell_fixer.hpp"
#endif

namespace po = boost::program_options;
namespace fs = std::filesystem;
using namespace qac;

extern const char* const kQacDemoPage;  // generated from web/index.html

namespace {

constexpr const char* kVersion = "0.1.0";

struct UsageError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

void print_usage(std::ostream& os) {
  os << "qac " << kVersion << " - query autocomplete\n\n"
     << "usage: qac <command> [options]\n\n"
        "commands:\n"
        "  train      build a data directory from a text file (one query per line)\n"
        "  serve      run the REST server\n"
        "  complete   look a query up from the command line\n"
        "  inspect    show what a data directory contains\n"
        "  optimize   compact a data directory offline\n"
        "  version    print the version\n\n"
        "Run 'qac <command> --help' for the options of a command.\n";
}

// ---- shared option groups -----------------------------------------------------------

struct Common {
  std::string data_dir;
  std::string alphabet;
  std::string log_level = "info";
  std::string jamspell_model;
};

po::options_description common_options(Common& c, bool with_model) {
  po::options_description d("Common options");
  d.add_options()
      ("data-dir,d", po::value(&c.data_dir)->required(), "data directory (snapshot + write-ahead log)")
      ("alphabet,a", po::value(&c.alphabet), "alphabet file (default: $QAC_ALPHABET, the installed or bundled alphabet.txt)")
      ("log-level", po::value(&c.log_level)->default_value("info"), "debug | info | warn | error | off");
  if (with_model)
    d.add_options()("jamspell-model", po::value(&c.jamspell_model),
                    "JamSpell language model; enables correct=1 requests");
  return d;
}

po::variables_map parse(const std::string& command, std::vector<std::string> args,
                        po::options_description& desc, bool& help,
                        const po::positional_options_description* pos = nullptr) {
  po::options_description with_help;
  with_help.add(desc).add_options()("help,h", "show this help");
  po::variables_map vm;
  auto parser = po::command_line_parser(args).options(with_help);
  if (pos) parser.positional(*pos);
  try {
    po::store(parser.run(), vm);
    help = vm.count("help") > 0;
    if (help) {
      std::cout << "usage: qac " << command << " [options]\n\n" << with_help << "\n";
      return vm;
    }
    po::notify(vm);
  } catch (const po::error& e) {
    throw UsageError(std::string(e.what()) + " (see 'qac " + command + " --help')");
  }
  return vm;
}

void apply_log_level(const std::string& s) {
  if (s == "debug") set_log_level(LogLevel::Debug);
  else if (s == "info") set_log_level(LogLevel::Info);
  else if (s == "warn") set_log_level(LogLevel::Warn);
  else if (s == "error") set_log_level(LogLevel::Error);
  else if (s == "off") set_log_level(LogLevel::Off);
  else throw UsageError("unknown log level '" + s + "'");
}

std::string find_alphabet(const std::string& explicit_path) {
  if (!explicit_path.empty()) return explicit_path;
  if (const char* env = std::getenv("QAC_ALPHABET"); env && *env) return env;
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    const fs::path installed = exe.parent_path().parent_path() / "share" / "qac" / "alphabet.txt";
    if (fs::exists(installed, ec)) return installed.string();
  }
  const fs::path bundled = QAC_BUNDLED_ALPHABET;
  if (fs::exists(bundled, ec)) return bundled.string();
  throw std::runtime_error("no alphabet file found; pass --alphabet FILE");
}

Alphabet load_alphabet(const Common& c) {
  const std::string path = find_alphabet(c.alphabet);
  log_info("alphabet: " + path);
  return Alphabet::load(path);
}

std::shared_ptr<const SpellFixer> load_fixer(const Common& c) {
  if (c.jamspell_model.empty()) return nullptr;
#ifdef QAC_HAVE_JAMSPELL
  log_info("loading JamSpell model " + c.jamspell_model);
  return std::shared_ptr<const SpellFixer>(JamSpellFixer::load(c.jamspell_model));
#else
  throw std::runtime_error("this build has no JamSpell support (configured with QAC_WITH_JAMSPELL=OFF)");
#endif
}

std::string bytes_h(std::uint64_t b) {
  const char* unit[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double v = static_cast<double>(b);
  int i = 0;
  while (v >= 1024 && i < 4) v /= 1024, ++i;
  std::ostringstream os;
  os << std::fixed << std::setprecision(i ? 1 : 0) << v << ' ' << unit[i];
  return os.str();
}

// ---- train -------------------------------------------------------------------------------

int cmd_train(const std::vector<std::string>& args) {
  Common c;
  TrainOptions t;
  unsigned max_words = Limits{}.max_words, max_letters = Limits{}.max_word_letters,
           max_bytes = Limits{}.max_input_bytes;
  bool no_counts = false;
  po::options_description desc;
  desc.add(common_options(c, false));
  po::options_description own("Train options");
  own.add_options()
      ("input,i", po::value(&t.input)->required(), "queries file, one per line ('-' for stdin); optional \"<TAB>count\" suffix")
      ("no-counts", po::bool_switch(&no_counts), "treat a tab-separated number as part of the query")
      ("overwrite", po::bool_switch(&t.overwrite), "replace existing data in the data directory")
      ("max-words", po::value(&max_words)->default_value(max_words), "words per query")
      ("max-word-letters", po::value(&max_letters)->default_value(max_letters), "letters per word")
      ("max-input-bytes", po::value(&max_bytes)->default_value(max_bytes), "UTF-8 bytes per query")
      ("progress-every", po::value(&t.progress_every)->default_value(1000000), "log progress every N lines (0: never)");
  desc.add(own);
  po::positional_options_description pos;
  pos.add("input", 1);
  bool help = false;
  parse("train", args, desc, help, &pos);
  if (help) return 0;
  apply_log_level(c.log_level);

  t.data_dir = c.data_dir;
  t.counts = !no_counts;
  t.limits = {max_words, max_letters, max_bytes};
  t.progress = [](std::uint64_t n) { log_info("read " + std::to_string(n) + " lines"); };
  const Alphabet alphabet = load_alphabet(c);
  const TrainReport r = train(alphabet, t);

  std::cout << "lines read:         " << r.lines << "\n"
            << "accepted:           " << r.accepted << "\n"
            << "blank / separators: " << r.blank << "\n"
            << "rejected:           " << r.rejected << "\n"
            << "distinct queries:   " << r.index.global_queries << "\n"
            << "distinct words:     " << r.index.words << "\n"
            << "snapshot:           " << bytes_h(r.snapshot_bytes) << " in " << c.data_dir << "\n"
            << "took:               " << r.duration_ms << " ms\n";
  if (!r.samples.empty()) {
    std::cout << "\nrejected lines (first " << r.samples.size() << "):\n";
    for (const auto& s : r.samples)
      std::cout << "  line " << s.line << ": " << s.reason << ": " << s.text << "\n";
  }
  return 0;
}

// ---- serve -------------------------------------------------------------------------------

int cmd_serve(const std::vector<std::string>& args) {
  Common c;
  EngineConfig ec;
  HttpServerConfig hc;
  ApiConfig ac;
  std::string fsync = "interval", snapshot_mode = "fork";
  unsigned snapshot_wal_mb = static_cast<unsigned>(ec.snapshot_wal_bytes >> 20);
  unsigned wal_segment_mb = static_cast<unsigned>(ec.wal_segment_bytes >> 20);
  unsigned port = hc.port;
  bool no_admin = false, no_demo = false;
  std::size_t max_n = ac.max_n;

  po::options_description desc;
  desc.add(common_options(c, true));
  po::options_description net("Network");
  net.add_options()
      ("listen", po::value(&hc.address)->default_value(hc.address), "address to listen on")
      ("port,p", po::value(&port)->default_value(port), "TCP port")
      ("io-threads", po::value(&hc.io_threads)->default_value(hc.io_threads), "asio I/O threads")
      ("workers", po::value(&hc.worker_threads)->default_value(0), "request worker threads (0: hardware concurrency)")
      ("max-connections", po::value(&hc.max_connections)->default_value(hc.max_connections), "simultaneous connections")
      ("max-body", po::value(&hc.max_body_bytes)->default_value(hc.max_body_bytes), "largest request body, bytes")
      ("idle-timeout", po::value(&hc.idle_timeout_s)->default_value(hc.idle_timeout_s), "keep-alive idle timeout, seconds")
      ("cors", po::bool_switch(&ac.cors), "send Access-Control-Allow-Origin: *")
      ("no-admin", po::bool_switch(&no_admin), "disable /v1/snapshot and /v1/optimize")
      ("no-demo", po::bool_switch(&no_demo), "do not serve the demo page at /")
      ("max-n", po::value(&max_n)->default_value(max_n), "largest n / m a request may ask for");
  desc.add(net);
  po::options_description dur("Durability and snapshots");
  dur.add_options()
      ("fsync", po::value(&fsync)->default_value(fsync), "WAL fsync policy: always | interval | never")
      ("fsync-interval-ms", po::value(&ec.fsync_interval_ms)->default_value(ec.fsync_interval_ms), "interval policy period")
      ("wal-segment-mb", po::value(&wal_segment_mb)->default_value(wal_segment_mb), "WAL segment size")
      ("snapshot-interval", po::value(&ec.snapshot_interval_s)->default_value(ec.snapshot_interval_s), "automatic snapshot every N seconds (0: off)")
      ("snapshot-wal-mb", po::value(&snapshot_wal_mb)->default_value(snapshot_wal_mb), "automatic snapshot when the WAL reaches N MiB (0: off)")
      ("snapshot-mode", po::value(&snapshot_mode)->default_value(snapshot_mode), "fork (copy-on-write child, no pauses) | inline (stop the world)");
  desc.add(dur);
  po::options_description tune("Search and cache");
  tune.add_options()
      ("cache-entries", po::value(&ec.cache_entries)->default_value(ec.cache_entries), "global result cache size (0: off)")
      ("cache-ttl-ms", po::value(&ec.cache_ttl_ms)->default_value(ec.cache_ttl_ms), "cache entry lifetime")
      ("max-visited", po::value(&ec.scan.max_visited)->default_value(ec.scan.max_visited), "global scan budget (queries visited)")
      ("max-user-visited", po::value(&ec.scan.max_user_visited)->default_value(ec.scan.max_user_visited), "user scan budget")
      ("max-candidates", po::value(&ec.scan.max_candidates)->default_value(ec.scan.max_candidates), "dictionary words tried for a partial last word");
  desc.add(tune);

  bool help = false;
  parse("serve", args, desc, help);
  if (help) return 0;
  apply_log_level(c.log_level);

  if (port > 65535) throw UsageError("--port must be at most 65535");
  hc.port = static_cast<unsigned short>(port);
  if (!parse_fsync_policy(fsync, ec.fsync)) throw UsageError("--fsync must be always, interval or never");
  if (!parse_snapshot_mode(snapshot_mode, ec.snapshot_mode))
    throw UsageError("--snapshot-mode must be fork or inline");
  ec.data_dir = c.data_dir;
  ec.snapshot_wal_bytes = static_cast<std::uint64_t>(snapshot_wal_mb) << 20;
  ec.wal_segment_bytes = static_cast<std::uint64_t>(std::max(1u, wal_segment_mb)) << 20;
  ac.max_n = ac.max_m = max_n;
  ac.allow_admin = !no_admin;
  if (!no_demo) ac.demo_page = kQacDemoPage;

  // Signals are blocked before any thread exists so that every thread inherits the
  // mask and the main thread can wait for them.
  sigset_t sigs;
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &sigs, nullptr);
  signal(SIGPIPE, SIG_IGN);

  const Alphabet alphabet = load_alphabet(c);
  Engine engine(alphabet, ec);
  if (auto fixer = load_fixer(c)) engine.set_spell_fixer(std::move(fixer));

  Api api(engine, ac);
  HttpServer server(api, hc);
  server.start();
  engine.start_background();
  log_info("listening on " + hc.address + ":" + std::to_string(server.port()) +
           (engine.has_spell_fixer() ? " (spell correction available)" : ""));

  int sig = 0;
  sigwait(&sigs, &sig);
  log_info(std::string("received ") + (sig == SIGINT ? "SIGINT" : "SIGTERM") + ", shutting down");

  server.stop();
  engine.stop_background();
  if (engine.stats().wal_bytes > 0) {
    const auto r = engine.snapshot();
    if (r.ok())
      log_info("final snapshot written (" + bytes_h(r.file_bytes) + ", " + std::to_string(r.duration_ms) + " ms)");
    else
      log_warn("final snapshot failed: " + r.message + " (the WAL is kept and will be replayed)");
  }
  return 0;
}

// ---- offline tools -----------------------------------------------------------------------

Engine open_offline(const Common& c, const Alphabet& alphabet) {
  EngineConfig ec;
  ec.data_dir = c.data_dir;
  ec.fsync = FsyncPolicy::Always;
  ec.snapshot_interval_s = 0;
  ec.snapshot_wal_bytes = 0;
  ec.cache_entries = 0;
  return Engine(alphabet, ec);
}

int cmd_complete(const std::vector<std::string>& args) {
  Common c;
  std::string query;
  std::size_t n = 10, m = 5;
  std::uint32_t user = 0;
  bool has_user = false, correct = false;
  std::string rank = "frequency";
  po::options_description desc;
  desc.add(common_options(c, true));
  po::options_description own("Lookup options");
  own.add_options()
      ("query,q", po::value(&query)->required(), "text typed so far")
      ("top-n,n", po::value(&n)->default_value(n), "top-N from the global pool")
      ("user,u", po::value<std::uint32_t>(), "also search this user's history")
      ("top-m,m", po::value(&m)->default_value(m), "top-M from the user's history")
      ("correct", po::bool_switch(&correct), "spell-correct the complete words (needs --jamspell-model)")
      ("rank", po::value(&rank)->default_value(rank),
       "order of the global candidates: frequency | avg_word_freq (mean frequency of the words of each candidate)");
  desc.add(own);
  bool help = false;
  const auto vm = parse("complete", args, desc, help);
  if (help) return 0;
  apply_log_level(c.log_level);
  if (vm.count("user")) user = vm["user"].as<std::uint32_t>(), has_user = true;

  const Alphabet alphabet = load_alphabet(c);
  Engine engine = open_offline(c, alphabet);
  if (auto fixer = load_fixer(c)) engine.set_spell_fixer(std::move(fixer));
  CompleteRequest req;
  req.query = query;
  req.n = n;
  req.m = m;
  req.correct = correct;
  if (rank == "avg_word_freq") req.rank = Index::GlobalRank::AverageWordFrequency;
  else if (rank != "frequency") throw UsageError("--rank must be frequency or avg_word_freq");
  if (has_user) req.user = user;
  const CompleteResult r = engine.complete(req);
  if (!r.ok()) throw std::runtime_error(r.message);
  if (r.corrected) std::cout << "corrected: " << *r.corrected << "\n";
  auto print = [](const char* title, const std::vector<Suggestion>& v) {
    std::cout << title << " (" << v.size() << ")\n";
    for (const auto& s : v) {
      std::cout << "  " << std::setw(10) << s.freq << "  #" << s.id << "  " << s.text;
      if (s.score) std::cout << "   (avg word freq " << std::fixed << std::setprecision(1) << *s.score << ")";
      std::cout << "\n";
    }
  };
  print("global", r.global);
  if (has_user) print("user", r.user);
  if (r.truncated) std::cout << "note: a scan budget ran out, results may be approximate\n";
  std::cout << "took " << r.took_us << " us\n";
  return 0;
}

int cmd_inspect(const std::vector<std::string>& args) {
  Common c;
  po::options_description desc;
  desc.add(common_options(c, false));
  bool help = false;
  parse("inspect", args, desc, help);
  if (help) return 0;
  apply_log_level(c.log_level);
  const Alphabet alphabet = load_alphabet(c);
  Engine engine = open_offline(c, alphabet);
  const EngineStats s = engine.stats();
  std::cout << "data directory:     " << c.data_dir << "\n"
            << "global queries:     " << s.index.global_queries << "\n"
            << "user entries:       " << s.index.user_entries << " (" << s.index.users << " users)\n"
            << "distinct words:     " << s.index.words << "\n"
            << "query ids used:     " << s.index.query_ids_allocated << " (" << s.index.dead_queries
            << " dead, reclaimed by optimize)\n"
            << "table memory:       " << bytes_h(s.index.arena_bytes) << "\n"
            << "snapshot:           " << bytes_h(s.snapshot_bytes) << ", covers sequence " << s.snapshot_seq << "\n"
            << "write-ahead log:    " << bytes_h(s.wal_bytes) << ", last sequence " << s.last_seq << "\n"
            << "replayed at open:   " << s.recovered_records << " records\n";
  return 0;
}

int cmd_optimize(const std::vector<std::string>& args) {
  Common c;
  po::options_description desc;
  desc.add(common_options(c, false));
  bool help = false;
  parse("optimize", args, desc, help);
  if (help) return 0;
  apply_log_level(c.log_level);
  const Alphabet alphabet = load_alphabet(c);
  Engine engine = open_offline(c, alphabet);
  const auto r = engine.optimize();
  if (!r.ok()) throw std::runtime_error(r.message);
  std::cout << "compacted " << r.dead_queries_before << " dead queries; snapshot " << bytes_h(r.file_bytes)
            << ", sequence " << r.last_seq << ", " << r.duration_ms << " ms\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage(std::cerr);
    return 2;
  }
  const std::string command = argv[1];
  const std::vector<std::string> args(argv + 2, argv + argc);
  try {
    if (command == "help" || command == "--help" || command == "-h") return print_usage(std::cout), 0;
    if (command == "version" || command == "--version") return std::cout << "qac " << kVersion << "\n", 0;
    if (command == "train") return cmd_train(args);
    if (command == "serve") return cmd_serve(args);
    if (command == "complete") return cmd_complete(args);
    if (command == "inspect") return cmd_inspect(args);
    if (command == "optimize") return cmd_optimize(args);
    std::cerr << "qac: unknown command '" << command << "'\n\n";
    print_usage(std::cerr);
    return 2;
  } catch (const UsageError& e) {
    std::cerr << "qac " << command << ": " << e.what() << "\n";
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "qac " << command << ": error: " << e.what() << "\n";
    return 1;
  }
}
