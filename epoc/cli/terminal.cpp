#include "terminal.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <algorithm>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#endif

namespace epoc::cli {
namespace {

std::atomic<bool> g_interrupted{false};

extern "C" void on_sigint(int) {
    // Only touches an atomic flag; safe from a signal handler.
    g_interrupted.store(true, std::memory_order_relaxed);
}

}  // namespace

bool Terminal::stdout_is_tty() noexcept {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(STDOUT_FILENO) != 0;
#endif
}

bool Terminal::stderr_is_tty() noexcept {
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

int Terminal::width() noexcept {
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (out != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(out, &info)) {
        return info.srWindow.Right - info.srWindow.Left + 1;
    }
    return 0;
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return static_cast<int>(ws.ws_col);
    }
    return 0;
#endif
}

Terminal::Terminal() {
    if (!stdout_is_tty()) {
        ansi_ = false;
        return;
    }
#ifdef _WIN32
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode)) {
        saved_mode_ = mode;
        mode_saved_ = true;
        if (SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            ansi_ = true;
        }
    }
#else
    ansi_ = true;
#endif
}

Terminal::~Terminal() {
    if (ansi_) show_cursor(true);
#ifdef _WIN32
    if (mode_saved_) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out != INVALID_HANDLE_VALUE) SetConsoleMode(out, saved_mode_);
    }
#endif
}

void Terminal::home() const {
    if (ansi_) std::fputs("\x1b[H", stdout);
}

void Terminal::clear() const {
    if (ansi_) std::fputs("\x1b[2J\x1b[H", stdout);
}

void Terminal::show_cursor(bool visible) const {
    if (ansi_) std::fputs(visible ? "\x1b[?25h" : "\x1b[?25l", stdout);
}

void install_interrupt_handler() {
    std::signal(SIGINT, on_sigint);
#ifdef SIGTERM
    std::signal(SIGTERM, on_sigint);
#endif
#ifdef SIGBREAK
    // Windows Ctrl+Break. On Windows these handlers run on a separate CRT
    // thread while the main thread sits in a blocking read, which is fine:
    // the handler only sets an atomic flag, and the main thread picks it up
    // when its read times out.
    std::signal(SIGBREAK, on_sigint);
#endif
}

bool interrupted() noexcept {
    return g_interrupted.load(std::memory_order_relaxed);
}

std::string bar(int value, int full_scale, int width) {
    if (width <= 0) return {};
    if (full_scale <= 0) return std::string(static_cast<std::size_t>(width), '-');
    const int clamped = std::clamp(value, 0, full_scale);
    const int filled = (clamped * width) / full_scale;
    std::string s;
    s.reserve(static_cast<std::size_t>(width));
    s.append(static_cast<std::size_t>(filled), '#');
    s.append(static_cast<std::size_t>(width - filled), '-');
    return s;
}

}  // namespace epoc::cli
