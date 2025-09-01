// TorFrog — tiny BitTorrent client in C++ (libtorrent-rasterbar)
// LEGAL USE ONLY. Download/share only content you are legally allowed to.
// Fancy-but-small TUI: ANSI colors, animated spinner, Unicode progress bar, ETA, ASCII sparkline.
// Built-in public trackers, sequential download ON by default, optional rate limits.
//
// 🔢 Auto-versioning
//  - By default the version shows the compile date/time.
//  - You can override it:  g++ ... -DTORFROG_VERSION="\"1.6.0+42\""
//    (Mind the escaped quotes when passing through shells.)
//
// Flags:
//   --help / -h / -?        Show help and exit
//   --version               Show version and exit
//   --no-trackers           Do not add built-in public trackers
//   --no-color              Disable ANSI colors
//   --no-clear              Do not clear screen (print a stream of status blocks)
//   --ascii                 ASCII-only UI (no Unicode blocks/spinner)
//   --no-seq / --seq        Disable/Enable sequential download (default: enabled)
//   --max-down <KiBps>      Download rate limit in KiB/s (0 = unlimited, default)
//   --max-up   <KiBps>      Upload   rate limit in KiB/s (0 = unlimited, default)
//   --bar-width <cols>      Progress bar width (default: 48)
//   --save <dir>            Output directory (default: ./downloads)
// Usage:
//   torfrog <magnet_or_.torrent> [options]

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <deque>
#include <csignal>
#include <chrono>
#include <thread>
#include <iomanip>
#include <filesystem>
#include <cstdlib>
#include <sstream>
#include <cmath>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace lt = libtorrent;

#ifndef TORFROG_VERSION
#  define TORFROG_VERSION __DATE__ " " __TIME__
#endif
static constexpr const char* kVersion = TORFROG_VERSION;

static volatile std::sig_atomic_t g_stop = 0;
void on_sigint(int) { g_stop = 1; }

static bool is_magnet(const std::string& s) { return s.rfind("magnet:?", 0) == 0; }

// --- ANSI helpers ----------------------------------------------------------
static bool g_color = true;   // colors on/off
static bool g_clear = true;   // full-screen TUI vs block stream
static bool g_ascii = false;  // ASCII-only UI

#ifdef _WIN32
static void enable_vt_mode() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0; if (!GetConsoleMode(hOut, &mode)) return;
    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; SetConsoleMode(hOut, mode);
}
#else
static void enable_vt_mode() {}
#endif

static inline const char* C(const char* code){ return g_color ? code : ""; }
#define C_RESET   "\x1b[0m"
#define C_BOLD    "\x1b[1m"
#define C_DIM     "\x1b[2m"
#define C_RED     "\x1b[31m"
#define C_GREEN   "\x1b[32m"
#define C_YELLOW  "\x1b[33m"
#define C_BLUE    "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN    "\x1b[36m"
#define C_WHITE   "\x1b[37m"

static void clear_screen(){ if (g_clear) std::cout << "\x1b[2J\x1b[H"; }
static void home_cursor(){ if (g_clear) std::cout << "\x1b[H"; }
static void hide_cursor(){ if (g_clear) std::cout << "\x1b[?25l"; }
static void show_cursor(){ if (g_clear) std::cout << "\x1b[?25h"; }

// --- humanize --------------------------------------------------------------
static std::string human_rate(double bps){
    const char* u[] = {"B/s","KB/s","MB/s","GB/s"}; int i=0; double x=bps;
    while (x>=1024.0 && i<3){ x/=1024.0; ++i; }
    std::ostringstream os; os<<std::fixed<<std::setprecision(i?1:0)<<x<<' '<<u[i]; return os.str();
}
static std::string human_size(std::int64_t b){
    const char* u[] = {"B","KB","MB","GB","TB"}; int i=0; double x=double(b);
    while (x>=1024.0 && i<4){ x/=1024.0; ++i; }
    std::ostringstream os; os<<std::fixed<<std::setprecision(i?1:0)<<x<<' '<<u[i]; return os.str();
}

static std::string format_eta(double seconds){
    if (!(seconds > 0) || !std::isfinite(seconds)) return "ETA --:--";
    long s = long(seconds + 0.5);
    long h = s / 3600; s %= 3600; long m = s / 60; s %= 60;
    std::ostringstream os; os << "ETA ";
    if (h > 0) os << h << "h " << m << "m " << s << "s";
    else       os << m << "m " << s << "s";
    return os.str();
}

// Unicode progress bar with fractional blocks. ASCII fallback if g_ascii.
static std::string progress_bar(double percent, int width){
    percent = std::clamp(percent, 0.0, 100.0);
    double cells = (percent/100.0) * width;
    int full = int(cells);
    double frac = cells - full;

    std::string bar;
    bar.reserve(width + 2);
    bar.push_back('[');

    // always safe characters
    const char* partials[] = {"", "░", "▒", "▓"};
    for (int i=0; i<full; i++) bar += "█";
    if (full < width) {
        int idx = int(frac * 4.0 + 0.5);
        if (idx>3) idx=3;
        if (idx>0) { bar += partials[idx]; full++; }
    }
    for (int i=full; i<width; i++) bar.push_back(' ');

    bar.push_back(']');
    return bar;
}

// Spinner (Unicode braille or ASCII fallback)
static std::string spinner_frame(size_t n){
    if (g_ascii) {
        static const char* sp[] = {"-","\\","|","/"};
        return sp[n % 4];
    } else {
        static const char* sp[] = {"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
        return sp[n % 10];
    }
}

// ASCII sparkline for DL rate history (avoids tofu squares).
static std::string sparkline(const std::deque<double>& values, int width){
    if (values.empty() || width <= 0) return std::string();
    double mx = 0.0; for (double v : values) if (v > mx) mx = v;
    if (mx <= 0.0) return std::string(width, ' ');
    const char* levels = " .:-=+*#%@"; // 10 levels
    std::string out; out.reserve(width);
    int n = std::min<int>(width, (int)values.size());
    for (int i=(int)values.size()-n; i<(int)values.size(); ++i){
        double v = values[i];
        if (v <= 0.0) { out.push_back(' '); continue; }
        double r = v / mx; int idx = (int)std::floor(r * 9.0);
        if (idx < 0) idx = 0; if (idx > 9) idx = 9;
        out.push_back(levels[idx]);
    }
    if ((int)out.size() < width) out = std::string(width - out.size(), ' ') + out;
    return out;
}

// --- help ------------------------------------------------------------------
static void print_help(const char* prog){
    std::cout << "TorFrog " << kVersion << " — tiny BitTorrent client (libtorrent-rasterbar)\n"
              << "LEGAL USE ONLY.\n\n"
              << "Usage:\n  " << prog << " <magnet_or_.torrent> [options]\n\n"
              << "Options:\n"
              << "  --help, -h, -?        Show this help and exit\n"
              << "  --version             Show version and exit\n"
              << "  --save <dir>          Output directory (default: ./downloads)\n"
              << "  --no-trackers         Do not add built-in public trackers\n"
              << "  --no-color            Disable ANSI colors\n"
              << "  --no-clear            Do not clear the screen (single updating blocks)\n"
              << "  --ascii               ASCII-only UI (no Unicode blocks/spinner)\n"
              << "  --seq | --no-seq      Enable/disable sequential download (default: enabled)\n"
              << "  --max-down <KiBps>    Download rate limit in KiB/s (0 = unlimited)\n"
              << "  --max-up   <KiBps>    Upload   rate limit in KiB/s (0 = unlimited)\n"
              << "  --bar-width <cols>    Progress bar width (default: 48)\n\n"
              << "Examples:\n"
              << "  " << prog << " \"magnet:?xt=urn:btih:HASH...\" --save downloads\n"
              << "  " << prog << " myfile.torrent --no-clear --no-color --ascii --bar-width 60\n";
}

int main(int argc, char** argv) {
#ifdef _WIN32
    enable_vt_mode();
#endif
    std::signal(SIGINT, on_sigint); std::signal(SIGTERM, on_sigint);

    if (argc < 2) { print_help(argv[0]); return 1; }

    // quick flags that don't require a torrent argument
    std::string first = argv[1];
    if (first == "--help" || first == "-h" || first == "-?") { print_help(argv[0]); return 0; }
    if (first == "--version") { std::cout << "TorFrog " << kVersion << "\n"; return 0; }

    std::string input = argv[1]; bool add_trackers = true; std::string save_path = "downloads";
    bool sequential = true; // default: ON (stream-friendly)
    std::int64_t dl_limit_bps = 0; // 0 = unlimited
    std::int64_t ul_limit_bps = 0; // 0 = unlimited
    int bar_width = 48;

    auto parse_kibps = [](const std::string& s){
        if (s.empty()) return (std::int64_t)0;
        char suf = s.back(); std::string num = s; int pow2 = 0;
        if (suf=='k'||suf=='K'||suf=='m'||suf=='M'||suf=='g'||suf=='G'){
            num = s.substr(0, s.size()-1);
            if (suf=='m'||suf=='M') pow2 = 1; else if (suf=='g'||suf=='G') pow2 = 2;
        }
        long long val = std::strtoll(num.c_str(), nullptr, 10);
        if (val < 0) val = 0; std::int64_t kib = val; for(int i=0;i<pow2;i++) kib*=1024; return kib*1024; };

    for (int i=2; i<argc; ++i){
        std::string a = argv[i];
        if (a == "--no-trackers") add_trackers = false;
        else if (a == "--no-color") g_color = false;
        else if (a == "--no-clear") g_clear = false;
        else if (a == "--ascii") g_ascii = true;
        else if (a == "--no-seq") sequential = false;
        else if (a == "--seq") sequential = true;
        else if (a == "--save" && i+1 < argc) { save_path = argv[++i]; }
        else if (a == "--max-down" && i+1 < argc) { dl_limit_bps = parse_kibps(argv[++i]); }
        else if (a == "--max-up"   && i+1 < argc) { ul_limit_bps = parse_kibps(argv[++i]); }
        else if (a == "--bar-width" && i+1 < argc) { bar_width = std::max(10, std::atoi(argv[++i])); }
        else { std::cerr << "Unknown option: " << a << "\n\n"; print_help(argv[0]); return 1; }
    }

    if (!fs::exists(save_path)) fs::create_directories(save_path);

    // Session settings
    lt::settings_pack pack;
    pack.set_str(lt::settings_pack::user_agent, std::string("TorFrog/") + kVersion + " (libtorrent)");
    pack.set_bool(lt::settings_pack::enable_dht, true);
    pack.set_bool(lt::settings_pack::enable_lsd, true);
    pack.set_bool(lt::settings_pack::enable_upnp, true);
    pack.set_bool(lt::settings_pack::enable_natpmp, true);
    pack.set_int(lt::settings_pack::alert_mask, lt::alert_category::all);

    // Rate limits (0 = unlimited)
    pack.set_int(lt::settings_pack::download_rate_limit, static_cast<int>(dl_limit_bps));
    pack.set_int(lt::settings_pack::upload_rate_limit,   static_cast<int>(ul_limit_bps));

    // Optional: bump some capacities
    pack.set_int(lt::settings_pack::connections_limit, 500);
    pack.set_int(lt::settings_pack::active_downloads, 500);
    pack.set_int(lt::settings_pack::active_seeds, 500);

    lt::session ses(pack);

    lt::add_torrent_params atp; atp.save_path = save_path;
    try {
        if (is_magnet(input)) { atp = lt::parse_magnet_uri(input); atp.save_path = save_path; }
        else {
            lt::error_code ec; auto ti = std::make_shared<lt::torrent_info>(input, ec);
            if (ec) { std::cerr << "Failed to open .torrent: " << ec.message() << "\n"; return 2; }
            atp.ti = ti; atp.save_path = save_path;
        }
    } catch (const std::exception& e) { std::cerr << "Invalid input: " << e.what() << "\n"; return 3; }

    if (add_trackers) {
        static const char* trackers[] = {
            "udp://tracker.opentrackr.org:1337/announce",
            "udp://tracker.openbittorrent.com:6969/announce",
            "udp://tracker.internetwarriors.net:1337/announce",
            "udp://tracker.leechers-paradise.org:6969/announce",
            "udp://tracker.coppersurfer.tk:6969/announce"
        };
        for (auto* t : trackers) atp.trackers.push_back(t);
    }

    lt::torrent_handle h = ses.add_torrent(atp);

    // sequential download (default: ON) using flags to avoid deprecated API
    if (sequential) h.set_flags(lt::torrent_flags::sequential_download);
    else            h.unset_flags(lt::torrent_flags::sequential_download);

    if (g_clear) { clear_screen(); hide_cursor(); }

    bool finished = false; auto last_print = std::chrono::steady_clock::now();
    ses.post_torrent_updates();

    // Animation state
    size_t spin = 0;
    std::deque<double> dl_hist; // bytes/s history for sparkline
    const int spark_w = 30;     // width of sparkline area

    while (!g_stop) {
        std::vector<lt::alert*> alerts; ses.pop_alerts(&alerts);
        for (lt::alert* a : alerts) {
            if (auto* er = lt::alert_cast<lt::torrent_error_alert>(a)) {
                std::cerr << "[error] " << er->message() << "\n";
            } else if (auto* fin = lt::alert_cast<lt::torrent_finished_alert>(a)) {
                finished = true; fin->handle.save_resume_data(lt::torrent_handle::save_info_dict);
            }
        }

        auto st = h.status();
        auto now = std::chrono::steady_clock::now();
        if (now - last_print > std::chrono::milliseconds(120)) {
            last_print = now; spin++;
            double prog = st.progress_ppm / 10000.0; // 0..100
            std::int64_t remaining = st.total_wanted > st.total_done ? (st.total_wanted - st.total_done) : 0;
            double eta_seconds = (st.download_rate > 0.0 && remaining > 0) ? double(remaining) / st.download_rate : -1.0;

            // update history for sparkline
            dl_hist.push_back(st.download_rate);
            if ((int)dl_hist.size() > 200) dl_hist.pop_front();
            std::string spark = sparkline(dl_hist, spark_w);

            std::string bar = progress_bar(prog, bar_width);
            std::string spinch = spinner_frame(spin);

            if (g_clear) home_cursor();
            std::cout
                << C(C_BOLD) << C(C_GREEN) << spinch << " TorFrog" << C(C_RESET)
                << " " << kVersion << " — mini BitTorrent client\n"
                << C(C_DIM)  << "Save: " << fs::absolute(save_path).string() << C(C_RESET) << "\n"
                << (g_color?"\x1b[32m":"") << std::string(std::max(60, bar_width+12),'=') << C(C_RESET) << "\n"
                << C(C_YELLOW) << "Progress: " << std::fixed << std::setprecision(1) << prog << "% " << C(C_RESET)
                << C(C_WHITE) << bar << C(C_RESET) << "  " << C(C_WHITE) << format_eta(eta_seconds) << C(C_RESET) << "\n"
                << C(C_GREEN)  << "DL: " << human_rate(st.download_rate) << C(C_RESET)
                << "    " << C(C_MAGENTA) << "UP: " << human_rate(st.upload_rate) << C(C_RESET)
                << "    " << C(C_CYAN) << "Peers: " << st.num_peers << C(C_RESET) << "\n"
                << C(C_DIM)  << "DL trend: " << spark << C(C_RESET) << "\n"
                << C(C_BLUE)   << "Done: " << human_size(st.total_done) << C(C_RESET)
                << " / " << C(C_BLUE) << human_size(st.total_wanted) << C(C_RESET) << "\n"
                << C(C_WHITE)  << "Seq: " << (sequential?"ON":"OFF") << C(C_RESET)
                << "    Limits: DL " << (dl_limit_bps? human_rate(dl_limit_bps): std::string("unlimited"))
                << ", UP " << (ul_limit_bps? human_rate(ul_limit_bps): std::string("unlimited")) << C(C_RESET) << "\n"
                << (g_color?"\x1b[36m":"") << std::string(std::max(60, bar_width+12),'=') << C(C_RESET) << "\n"
                << C(C_DIM)    << "Name: " << st.name << C(C_RESET) << "\n";

            if (!g_clear) std::cout << "\n"; // block stream mode
            std::cout.flush();
        }

        if (finished) break;
        ses.post_torrent_updates();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    }

    if (g_clear) { show_cursor(); }

    if (g_stop) {
        std::cout << "Goodbye, see you soon." << std::endl;
    } else if (finished) {
        std::cout << "[done] Finished." << std::endl;
    } else {
        std::cout << "[signal] Exiting..." << std::endl;
    }
    return 0;
}
