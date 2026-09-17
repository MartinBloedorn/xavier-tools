// Small cross-platform terminal helper for the live view.
//
// Output is deliberately restricted to 7-bit ASCII (no box-drawing glyphs, no
// micro sign) so the display is identical on a Windows console, a Linux
// terminal and a macOS terminal without codepage or locale negotiation.

#ifndef XAVIER_EPOC_CLI_TERMINAL_HPP
#define XAVIER_EPOC_CLI_TERMINAL_HPP

#include <string>

namespace epoc::cli {

/// Enables ANSI/VT escape processing for the lifetime of the object and puts
/// the terminal back the way it was on destruction. On Windows this flips
/// ENABLE_VIRTUAL_TERMINAL_PROCESSING, which is a no-op on Windows 10 1511+
/// but is not on by default for a console application.
class Terminal {
public:
    Terminal();
    ~Terminal();

    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    /// False when escape sequences are unavailable (e.g. output redirected to
    /// a file, or an old console); callers should fall back to plain lines.
    bool supports_ansi() const noexcept { return ansi_; }

    /// True when stdout is an interactive terminal rather than a pipe/file.
    static bool stdout_is_tty() noexcept;

    /// Same for stderr. Status chatter goes to stderr so that redirecting
    /// stdout to a data file stays clean, but an in-place updating status
    /// line is only appropriate when stderr is a terminal.
    static bool stderr_is_tty() noexcept;

    /// Terminal width in columns, or 0 if it cannot be determined. Queried
    /// live rather than cached, so resizing mid-run is picked up.
    static int width() noexcept;

    /// Move the cursor to the top-left corner without clearing scrollback.
    void home() const;
    /// Clear the whole visible screen and home the cursor.
    void clear() const;
    /// Show/hide the cursor, so a refreshing display does not flicker a caret.
    void show_cursor(bool visible) const;

private:
    bool ansi_ = false;
#ifdef _WIN32
    unsigned long saved_mode_ = 0;
    bool mode_saved_ = false;
#endif
};

/// Install a SIGINT (Ctrl+C) handler. `interrupted()` then reports whether it
/// has fired, so the read loop can shut down cleanly instead of being killed.
void install_interrupt_handler();
bool interrupted() noexcept;

/// A fixed-width ASCII bar, e.g. "####------", for `value` scaled against
/// `full_scale` and rendered in `width` characters.
std::string bar(int value, int full_scale, int width);

}  // namespace epoc::cli

#endif  // XAVIER_EPOC_CLI_TERMINAL_HPP
