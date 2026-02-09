# TorFrog — tiny BitTorrent client

![TorFrog Logo](src/logo.png)

TorFrog is a **minimal, fast, and animated BitTorrent client** written in modern C++ using [libtorrent-rasterbar](https://www.libtorrent.org/).
It is designed to be **small, easy-to-use, and complete**: a single binary with a smooth TUI, sequential download by default, ETA display, progress bar, download trends, and optional built-in public trackers.

👉 [**Download TorFrog v1.7.0 (Linux x86_64)**](https://github.com/victormeloasm/TorFrog/releases/download/v1.7.0/TorFrog-1.0.0-linux-x86_64.tar.gz)

---

## What’s new in v1.7.0

TorFrog v1.7.0 focuses on **extreme size optimization without breaking the UI**:

* Ultra-compact Linux build (tiny stripped ELF).
* “Nuclear” console pipeline: output via **low-level POSIX I/O** (write), no iostream overhead.
* Stable **ASCII-safe UI**: progress bar never breaks due to Unicode/font/terminal issues.
* Render loop engineered to avoid heavy STL structures and keep refresh smooth.

---

## Features

* **Animated TUI**: spinner, progress bar, ETA, DL/UP rates, peer count.
* **Sequential download ON by default** (great for streaming).
* **DL trend sparkline** (ASCII-based, terminal-safe).
* **Graceful exit message** on `Ctrl+C` (clean shutdown UX).
* **Optional built-in public trackers** (disable with `--no-trackers`).
* **Bandwidth control**:

  * `--max-down <KiBps>` (0 = unlimited)
  * `--max-up <KiBps>` (0 = unlimited)
* **Layout and output controls**:

  * `--bar-width <cols>`
  * `--no-clear` (block stream mode)
  * `--no-color`
  * `--ascii` (kept for compatibility; UI is ASCII-safe regardless)
* **Integrated help/version**:

  * `--help` / `--version`
  * auto-help when run with no arguments
* **Auto-versioning** supported (`__DATE__ __TIME__`), with optional override via `-DTORFROG_VERSION="\"...\""`.

---

## Screenshot

![TorFrog running](src/wk.png)

---

## Download & Install (Linux)

Download the release `.tar.gz`, extract it, and run:

```bash
tar xzf TorFrog-1.0.0-linux-x86_64.tar.gz
chmod +x torfrog
./torfrog --version
```

If you want a dedicated folder:

```bash
mkdir -p TorFrog
tar xzf TorFrog-1.0.0-linux-x86_64.tar.gz -C TorFrog
cd TorFrog
chmod +x torfrog
./torfrog --help
```

---

## Build Instructions (Ubuntu 24.04)

### Dependencies

```bash
sudo apt update
sudo apt install -y clang lld pkg-config \
  libtorrent-rasterbar-dev libboost-system-dev
```

### Compile (size-oriented, Clang + lld)

This is the recommended “small binary” build:

```bash
clang++ torfrog.cpp -std=c++20 \
  -Oz -DNDEBUG -pipe \
  -ffunction-sections -fdata-sections \
  -fno-exceptions -fno-rtti \
  -fno-unwind-tables -fno-asynchronous-unwind-tables \
  -fuse-ld=lld \
  -Wl,--gc-sections -Wl,--icf=all -Wl,--as-needed -Wl,--build-id=none \
  -s -o torfrog \
  $(pkg-config --cflags --libs libtorrent-rasterbar)
```

Optional: strip extra note/comment sections (sometimes saves a few more bytes):

```bash
llvm-strip --strip-all \
  --remove-section=.comment \
  --remove-section=.note.gnu.build-id \
  --remove-section=.note.ABI-tag \
  torfrog
```

> Notes:
>
> * Binary size varies by distro/toolchain/lib versions.
> * TorFrog is **dynamically linked** (libtorrent remains an external runtime dependency), which is how the ELF stays tiny.

---

## Usage

Basic syntax:

```bash
torfrog <magnet_or_torrent_file> [options]
```

Examples:

```bash
# Default (sequential ON, no limits)
./torfrog "magnet:?xt=urn:btih:YOUR_HASH..." --save downloads

# With bandwidth limits (~10 MiB/s down, 2 MiB/s up)
./torfrog "magnet:?xt=urn:btih:YOUR_HASH..." --max-down 10m --max-up 2m

# Wider bar + no clear screen (log/block mode)
./torfrog "magnet:?xt=urn:btih:YOUR_HASH..." --bar-width 60 --no-clear

# Disable colors (useful for logs / CI)
./torfrog "magnet:?xt=urn:btih:YOUR_HASH..." --no-color

# Show help
./torfrog --help
```

---

## Options

* `--help`, `-h`, `-?` : Show help and exit
* `--version` : Show version and exit
* `--save <dir>` : Output directory (default: `./downloads`)
* `--no-trackers` : Do not add built-in public trackers
* `--no-color` : Disable ANSI colors
* `--no-clear` : Do not clear the screen (block stream mode)
* `--ascii` : Compatibility flag (UI is ASCII-safe regardless)
* `--seq` / `--no-seq` : Enable/disable sequential download (default: ON)
* `--max-down <KiBps>` : Download rate limit in KiB/s (0 = unlimited)
* `--max-up <KiBps>` : Upload rate limit in KiB/s (0 = unlimited)
* `--bar-width <cols>` : Width of the progress bar (default: 48)

---

## License

TorFrog is released under the MIT License.
Use only for **legal torrents** and content you are allowed to download/share.

---

🐸 *TorFrog — tiny, fast, sapudo.*


