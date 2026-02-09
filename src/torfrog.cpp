// TorFrog  tiny BitTorrent client in C++ (libtorrent-rasterbar)
// LEGAL USE ONLY. Download/share only content you are legally allowed to.
//
// Nuclear Linux build:
//   - No iostream, no filesystem, no deque
//   - No try/catch (error_code only)
//   - Output via write()
//   - ASCII-only progress bar (fixed width, no Unicode glitches)
//
// Flags:
//   --help / -h / -?        Show help and exit
//   --version               Show version and exit
//   --no-trackers           Do not add built-in public trackers
//   --no-color              Disable ANSI colors
//   --no-clear              Do not clear screen (print status blocks sequentially)
//   --ascii                 Keep (compat); UI is ASCII-safe regardless
//   --no-seq / --seq        Disable/Enable sequential download (default: enabled)
//   --max-down <KiBps>      Download rate limit in KiB/s (0 = unlimited)
//   --max-up   <KiBps>      Upload   rate limit in KiB/s (0 = unlimited)
//   --bar-width <cols>      Progress bar width (default: 48)
//   --save <dir>            Output directory (default: ./downloads)
//
// Usage:
//   torfrog <magnet_or_.torrent> [options]

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>

#include <string>
#include <vector>
#include <csignal>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstddef>
#include <cerrno>
#include <cstring>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace lt = libtorrent;

#ifndef TORFROG_VERSION
#  define TORFROG_VERSION __DATE__ " " __TIME__
#endif
static constexpr const char* kVersion = TORFROG_VERSION;

// ---------------------------------------------------------------------
// Low-level output helpers
// ---------------------------------------------------------------------

static void write_all_fd(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        int chunk = (int)(len - off);
        if (chunk > (1 << 20)) chunk = (1 << 20);
        int n = (int)::write(fd, data + off, (size_t)chunk);
        if (n <= 0) break;
        off += (std::size_t)n;
    }
}

static void write_cstr_fd(int fd, const char* s) {
    if (!s) return;
    std::size_t len = 0;
    while (s[len] != '\0') ++len;
    if (len) write_all_fd(fd, s, len);
}

static void write_str_fd(int fd, const std::string& s) {
    if (!s.empty()) write_all_fd(fd, s.data(), s.size());
}

static void print_line_out(const std::string& s) {
    write_str_fd(1, s);
    write_cstr_fd(1, "\n");
}

static void print_line_err(const std::string& s) {
    write_str_fd(2, s);
    write_cstr_fd(2, "\n");
}

// ---------------------------------------------------------------------
// Global flags & signals
// ---------------------------------------------------------------------

static volatile std::sig_atomic_t g_stop = 0;
static void on_sigint(int) { g_stop = 1; }

static bool is_magnet(const std::string& s) {
    return s.rfind("magnet:?", 0) == 0;
}

// --- ANSI helpers ----------------------------------------------------------
static bool g_color = true;
static bool g_clear = true;
static bool g_ascii = false; // compat flag; UI is ASCII-safe anyway

static inline const char* C(const char* code) { return g_color ? code : ""; }
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN    "\x1b[36m"
#define C_WHITE   "\x1b[37m"

static void clear_screen() {
    if (!g_clear) return;
    write_cstr_fd(1, "\x1b[2J\x1b[H");
}

// ---------------------------------------------------------------------
// Formatting helpers (no iostream)
// ---------------------------------------------------------------------

static double clamp_double(double v, double lo, double hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static std::string fmt_i64(std::int64_t x) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%lld", (long long)x);
    return std::string(buf);
}

static std::string fmt_u64(std::uint64_t x) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)x);
    return std::string(buf);
}

static std::string fmt_f1(double x) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.1f", x);
    return std::string(buf);
}

static std::string human_rate(double bps) {
    const char* u[] = {"B/s","KB/s","MB/s","GB/s"};
    int i = 0;
    double x = bps;
    while (x >= 1024.0 && i < 3) { x /= 1024.0; ++i; }
    char buf[64];
    if (i == 0) std::snprintf(buf, sizeof(buf), "%.0f %s", x, u[i]);
    else        std::snprintf(buf, sizeof(buf), "%.1f %s", x, u[i]);
    return std::string(buf);
}

static std::string human_size(std::int64_t b) {
    const char* u[] = {"B","KB","MB","GB","TB"};
    int i = 0;
    double x = (double)b;
    while (x >= 1024.0 && i < 4) { x /= 1024.0; ++i; }
    char buf[64];
    if (i == 0) std::snprintf(buf, sizeof(buf), "%.0f %s", x, u[i]);
    else        std::snprintf(buf, sizeof(buf), "%.1f %s", x, u[i]);
    return std::string(buf);
}

static std::string format_eta(double seconds) {
    if (!(seconds > 0.0) || !std::isfinite(seconds)) return "ETA --:--";
    long s = (long)(seconds + 0.5);
    long h = s / 3600; s %= 3600;
    long m = s / 60;   s %= 60;
    char buf[64];
    if (h > 0) std::snprintf(buf, sizeof(buf), "ETA %ldh %ldm %lds", h, m, s);
    else       std::snprintf(buf, sizeof(buf), "ETA %ldm %lds", m, s);
    return std::string(buf);
}

// ---------------------------------------------------------------------
// ASCII progress bar & spinner (no Unicode ever)
// ---------------------------------------------------------------------

static std::string progress_bar(double percent, int width) {
    percent = clamp_double(percent, 0.0, 100.0);
    int inner = width;
    if (inner < 10) inner = 10;

    double filled = (percent / 100.0) * (double)inner;
    int full = (int)(filled + 0.5);
    if (full < 0) full = 0;
    if (full > inner) full = inner;

    std::string bar;
    bar.reserve((std::size_t)inner + 2);
    bar.push_back('[');
    for (int i = 0; i < inner; ++i) bar.push_back(i < full ? '#' : ' ');
    bar.push_back(']');
    return bar;
}

static const char* spinner_frame(std::size_t n) {
    static const char* sp[] = {"-","\\","|","/"};
    return sp[n & 3];
}

// ---------------------------------------------------------------------
// Fixed-size DL history for sparkline (no deque)
// ---------------------------------------------------------------------

static constexpr int DLHIST_MAX = 200;

struct DlHist {
    double v[DLHIST_MAX];
    int size;
};

static void dl_hist_init(DlHist& h) {
    h.size = 0;
    for (int i = 0; i < DLHIST_MAX; ++i) h.v[i] = 0.0;
}

static void dl_hist_push(DlHist& h, double val) {
    if (h.size < DLHIST_MAX) {
        h.v[h.size++] = val;
    } else {
        std::memmove(&h.v[0], &h.v[1], sizeof(double) * (DLHIST_MAX - 1));
        h.v[DLHIST_MAX - 1] = val;
    }
}

static std::string sparkline(const DlHist& h, int width) {
    if (h.size <= 0 || width <= 0) return std::string();
    double mx = 0.0;
    for (int i = 0; i < h.size; ++i) if (h.v[i] > mx) mx = h.v[i];
    if (mx <= 0.0) return std::string((std::size_t)width, ' ');

    const char* levels = " .:-=+*#%@"; // 10
    std::string out;
    out.reserve((std::size_t)width);

    int n = (h.size < width) ? h.size : width;
    int start = h.size - n;
    for (int i = start; i < h.size; ++i) {
        double v = h.v[i];
        if (v <= 0.0) { out.push_back(' '); continue; }
        double r = v / mx;
        int idx = (int)std::floor(r * 9.0);
        if (idx < 0) idx = 0;
        if (idx > 9) idx = 9;
        out.push_back(levels[idx]);
    }
    if ((int)out.size() < width) out = std::string((std::size_t)(width - out.size()), ' ') + out;
    return out;
}

// ---------------------------------------------------------------------
// mkdir -p (no filesystem)
// ---------------------------------------------------------------------

static void mkdir_one(const char* p) {
    if (!p || !*p) return;
    if (::mkdir(p, 0755) != 0 && errno != EEXIST) {
        std::string msg = "mkdir failed: ";
        msg += p;
        msg += " errno=";
        msg += fmt_i64(errno);
        print_line_err(msg);
    }
}

static void ensure_dir_p(const std::string& path) {
    if (path.empty()) return;

    char tmp[4096];
    std::size_t n = path.size();
    if (n >= sizeof(tmp)) {
        print_line_err("save path too long");
        return;
    }
    std::memcpy(tmp, path.c_str(), n);
    tmp[n] = '\0';

    // Handle absolute paths: keep leading '/'
    for (std::size_t i = 1; i < n; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir_one(tmp);
            tmp[i] = '/';
        }
    }
    mkdir_one(tmp);
}

// ---------------------------------------------------------------------
// Help
// ---------------------------------------------------------------------

static void print_help(const char* prog) {
    std::string s;
    s.reserve(1024);
    s += "TorFrog ";
    s += kVersion;
    s += " - tiny BitTorrent client (libtorrent-rasterbar)\n";
    s += "LEGAL USE ONLY.\n\n";
    s += "Usage:\n  ";
    s += prog;
    s += " <magnet_or_.torrent> [options]\n\n";
    s += "Options:\n";
    s += "  --help, -h, -?        Show this help and exit\n";
    s += "  --version             Show version and exit\n";
    s += "  --save <dir>          Output directory (default: ./downloads)\n";
    s += "  --no-trackers         Do not add built-in public trackers\n";
    s += "  --no-color            Disable ANSI colors\n";
    s += "  --no-clear            Do not clear the screen (print status blocks)\n";
    s += "  --ascii               ASCII-only UI (kept for compat)\n";
    s += "  --seq | --no-seq      Enable/disable sequential download (default: enabled)\n";
    s += "  --max-down <KiBps>    Download rate limit in KiB/s (0 = unlimited)\n";
    s += "  --max-up   <KiBps>    Upload   rate limit in KiB/s (0 = unlimited)\n";
    s += "  --bar-width <cols>    Progress bar width (default: 48)\n\n";
    s += "Examples:\n  ";
    s += prog;
    s += " \"magnet:?xt=urn:btih:HASH...\" --save downloads\n";
    s += "  ";
    s += prog;
    s += " myfile.torrent --no-clear --no-color --bar-width 60\n";
    write_str_fd(1, s);
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------

int main(int argc, char** argv) {
    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    if (argc < 2) { print_help(argv[0]); return 1; }

    std::string first = argv[1];
    if (first == "--help" || first == "-h" || first == "-?") { print_help(argv[0]); return 0; }
    if (first == "--version") {
        std::string v = "TorFrog ";
        v += kVersion;
        v += "\n";
        write_str_fd(1, v);
        return 0;
    }

    std::string input = argv[1];
    bool add_trackers = true;
    std::string save_path = "downloads";
    bool sequential = true;
    std::int64_t dl_limit_bps = 0;
    std::int64_t ul_limit_bps = 0;
    int bar_width = 48;

    auto parse_kibps = [](const std::string& s) -> std::int64_t {
        if (s.empty()) return 0;
        char suf = s.back();
        std::string num = s;
        int pow2 = 0;
        if (suf=='k'||suf=='K'||suf=='m'||suf=='M'||suf=='g'||suf=='G') {
            num = s.substr(0, s.size()-1);
            if (suf=='m'||suf=='M') pow2 = 1;
            else if (suf=='g'||suf=='G') pow2 = 2;
        }
        long long val = std::strtoll(num.c_str(), nullptr, 10);
        if (val < 0) val = 0;
        std::int64_t kib = (std::int64_t)val;
        for (int i = 0; i < pow2; ++i) kib *= 1024;
        return kib * 1024;
    };

    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-trackers") add_trackers = false;
        else if (a == "--no-color") g_color = false;
        else if (a == "--no-clear") g_clear = false;
        else if (a == "--ascii") g_ascii = true;
        else if (a == "--no-seq") sequential = false;
        else if (a == "--seq") sequential = true;
        else if (a == "--save" && i+1 < argc) save_path = argv[++i];
        else if (a == "--max-down" && i+1 < argc) dl_limit_bps = parse_kibps(argv[++i]);
        else if (a == "--max-up"   && i+1 < argc) ul_limit_bps = parse_kibps(argv[++i]);
        else if (a == "--bar-width" && i+1 < argc) {
            int bw = std::atoi(argv[++i]);
            if (bw < 10) bw = 10;
            bar_width = bw;
        } else {
            std::string msg = "Unknown option: ";
            msg += a;
            msg += "\n\n";
            write_str_fd(2, msg);
            print_help(argv[0]);
            return 1;
        }
    }

    ensure_dir_p(save_path);

    // Session settings
    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::user_agent, std::string("TorFrog/") + kVersion + " (libtorrent)");
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);

    // Mask minimal-ish alerts (status + error). Finished alert is in status.
    pack.set_int(lt::settings_pack::alert_mask, lt::alert_category::status | lt::alert_category::error);

    // Rate limits (0 = unlimited)
    pack.set_int(lt::settings_pack::download_rate_limit, (int)dl_limit_bps);
    pack.set_int(lt::settings_pack::upload_rate_limit,   (int)ul_limit_bps);

    // Capacities
    pack.set_int(lt::settings_pack::connections_limit, 500);
    pack.set_int(lt::settings_pack::active_downloads, 500);
    pack.set_int(lt::settings_pack::active_seeds, 500);

    lt::session ses(pack);

    lt::add_torrent_params atp;
    atp.save_path = save_path;

    lt::error_code ec;
    if (is_magnet(input)) {
        atp = lt::parse_magnet_uri(input, ec);
        if (ec) { print_line_err(std::string("Invalid magnet: ") + ec.message()); return 3; }
        atp.save_path = save_path;
    } else {
        auto ti = std::make_shared<lt::torrent_info>(input, ec);
        if (ec) { print_line_err(std::string("Failed to open .torrent: ") + ec.message()); return 2; }
        atp.ti = ti;
        atp.save_path = save_path;
    }

    if (add_trackers) {
        static const char* trackers[] = {
            "udp://tracker.opentrackr.org:1337/announce",
            "udp://tracker.openbittorrent.com:6969/announce",
            "udp://tracker.internetwarriors.net:1337/announce",
            "udp://tracker.leechers-paradise.org:6969/announce",
            "udp://tracker.coppersurfer.tk:6969/announce"
        };
        for (const char* t : trackers) atp.trackers.push_back(t);
    }

    lt::error_code ec_add;
    lt::torrent_handle h = ses.add_torrent(atp, ec_add);
    if (ec_add) { print_line_err(std::string("Failed to add torrent: ") + ec_add.message()); return 4; }

    if (sequential) h.set_flags(lt::torrent_flags::sequential_download);
    else            h.unset_flags(lt::torrent_flags::sequential_download);

    bool finished = false;
    auto last_print = std::chrono::steady_clock::now();
    ses.post_torrent_updates();

    std::size_t spin = 0;
    DlHist dl_hist; dl_hist_init(dl_hist);
    const int spark_w = 30;

    while (!g_stop) {
        std::vector<lt::alert*> alerts;
        ses.pop_alerts(&alerts);

        for (lt::alert* a : alerts) {
            if (auto* er = lt::alert_cast<lt::torrent_error_alert>(a)) {
                print_line_err(std::string("[error] ") + er->message());
            } else if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
                finished = true;
            }
        }

        lt::torrent_status st = h.status();

        auto now = std::chrono::steady_clock::now();
        if (now - last_print > std::chrono::milliseconds(150)) {
            last_print = now;
            ++spin;

            double prog = st.progress_ppm / 10000.0;
            std::int64_t remaining = (st.total_wanted > st.total_done) ? (st.total_wanted - st.total_done) : 0;
            double eta_seconds =
                (st.download_rate > 0.0 && remaining > 0)
                ? (double)remaining / st.download_rate
                : -1.0;

            dl_hist_push(dl_hist, st.download_rate);
            std::string spark = sparkline(dl_hist, spark_w);

            std::string bar = progress_bar(prog, bar_width);
            const char* sp = spinner_frame(spin);

            if (g_clear) clear_screen();

            int line_w = bar_width + 18;
            if (line_w < 60) line_w = 60;
            std::string sep((std::size_t)line_w, '=');

            std::string out;
            out.reserve(2200);

            out += C(C_BOLD); out += C(C_GREEN);
            out += sp; out += " TorFrog";
            out += C(C_RESET);
            out += " "; out += kVersion;
            out += "\n";

            out += C(C_DIM);
            out += "Save: "; out += save_path;
            out += C(C_RESET);
            out += "\n";

            if (g_color) out += C_GREEN;
            out += sep; out += C(C_RESET); out += "\n";

            out += C(C_YELLOW);
            out += "Progress: "; out += fmt_f1(prog); out += "% ";
            out += C(C_RESET);
            out += C(C_WHITE);
            out += bar;
            out += C(C_RESET);
            out += "  ";
            out += C(C_WHITE);
            out += format_eta(eta_seconds);
            out += C(C_RESET);
            out += "\n";

            out += C(C_GREEN);
            out += "DL: "; out += human_rate(st.download_rate);
            out += C(C_RESET);
            out += "    ";
            out += C(C_MAGENTA);
            out += "UP: "; out += human_rate(st.upload_rate);
            out += C(C_RESET);
            out += "    ";
            out += C(C_CYAN);
            out += "Peers: "; out += fmt_i64(st.num_peers);
            out += C(C_RESET);
            out += "\n";

            out += C(C_DIM);
            out += "DL trend: "; out += spark;
            out += C(C_RESET);
            out += "\n";

            out += C(C_DIM);
            out += "Done: ";
            out += human_size(st.total_done);
            out += " / ";
            out += human_size(st.total_wanted);
            out += C(C_RESET);
            out += "\n";

            out += C(C_DIM);
            out += "Seq: ";
            out += (sequential ? "ON" : "OFF");
            out += "   Limits: DL ";
            out += (dl_limit_bps ? human_rate((double)dl_limit_bps) : std::string("unlimited"));
            out += "  UP ";
            out += (ul_limit_bps ? human_rate((double)ul_limit_bps) : std::string("unlimited"));
            out += C(C_RESET);
            out += "\n";

            if (g_color) out += C_CYAN;
            out += sep; out += C(C_RESET); out += "\n";

            out += C(C_DIM);
            out += "Name: ";
            out += st.name;
            out += C(C_RESET);
            out += "\n";

            if (!g_clear) out += "\n";

            write_str_fd(1, out);
        }

        if (finished) break;

        ses.post_torrent_updates();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    if (g_stop) print_line_out("Goodbye.");
    else if (finished) print_line_out("[done] Finished.");
    else print_line_out("[signal] Exiting...");

    return 0;
}
