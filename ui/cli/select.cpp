// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.marco@gmail.com>
 *
 * This file is part of batchpress — Interactive file selection UI.
 *
 * Uses ANSI escape codes for terminal UI:
 *   - Cursor movement, clear screen, color codes
 *   - Raw terminal mode for key capture (via termios on Unix)
 */

#include "select.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

namespace cli {

// ── Terminal raw mode ────────────────────────────────────────────────────────

static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void restore_terminal() {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_termios_saved = false;
    }
}

static void enable_raw_mode() {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);  // non-canonical, no echo
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    g_termios_saved = true;
    atexit(restore_terminal);
}

// ── ANSI helpers ─────────────────────────────────────────────────────────────

static void term_clear() {
    std::cout << "\033[2J\033[H";
}

static void term_hide_cursor() {
    std::cout << "\033[?25l";
}

static void term_show_cursor() {
    std::cout << "\033[?25h";
}

static void term_goto(int row, int col) {
    std::cout << "\033[" << row << ";" << col << "H";
}

static void term_clear_line() {
    std::cout << "\033[2K";
}

static void term_color(const char* code) {
    std::cout << "\033[" << code << "m";
}

static void term_reset_color() {
    std::cout << "\033[0m";
}

static std::string fmt_bytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024ULL * 1024) return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ULL * 1024 * 1024)
        return std::to_string(bytes / (1024ULL * 1024)) + " MB";
    double gb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream ss;
    ss.precision(2);
    ss << std::fixed << gb << " GB";
    return ss.str();
}

// ── Key reading ──────────────────────────────────────────────────────────────

enum class Key {
    Up, Down, Space, Enter, Escape,
    Tab, Backspace,
    CharA, CharI, CharQ,
    Unknown,
    Char  // any printable char (for filtering)
};

static Key read_key(char* ch_out = nullptr) {
    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return Key::Unknown;

    if (c == '\x1b') {  // Escape sequence
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return Key::Escape;
        if (seq[0] != '[') return Key::Escape;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return Key::Escape;

        switch (seq[1]) {
            case 'A': return Key::Up;
            case 'B': return Key::Down;
            case 'D': return Key::Up;    // sometimes used for left
            case 'C': return Key::Down;  // sometimes used for right
        }
        return Key::Unknown;
    }

    if (c == '\n' || c == '\r') return Key::Enter;
    if (c == ' ') return Key::Space;
    if (c == '\x7f' || c == 127) return Key::Backspace;
    if (c == '\t') return Key::Tab;
    if (c == 3 || c == 27) return Key::Escape;  // Ctrl+C or Esc

    if (c == 'a' || c == 'A') return Key::CharA;
    if (c == 'i' || c == 'I') return Key::CharI;
    if (c == 'q' || c == 'Q') return Key::CharQ;
    if (c == 'j' || c == 'J') return Key::Down;   // vim-style down
    if (c == 'k' || c == 'K') return Key::Up;      // vim-style up

    if (ch_out) *ch_out = c;

    if (c >= 32 && c < 127) {
        return Key::Char;  // printable character for filtering
    }

    return Key::Unknown;
}

// ── Select UI state ──────────────────────────────────────────────────────────

struct SelectItem {
    batchpress::FileItem file;
    bool selected = true;  // default: all selected
};

struct SelectState {
    std::vector<SelectItem> items;
    std::vector<size_t> filtered_indices;  // indices into items that match filter
    size_t cursor_pos = 0;
    std::string filter_text;
    bool all_selected = true;
};

static void rebuild_filter(SelectState& state) {
    state.filtered_indices.clear();
    for (size_t i = 0; i < state.items.size(); ++i) {
        const auto& fi = state.items[i].file;

        // Apply type filter
        // (type filter already applied before UI, so we show all here)

        // Apply min savings filter
        if (fi.savings_pct < 0.01) {
            // very small savings, still show but mark
        }

        // Apply text filter (substring match on filename)
        if (!state.filter_text.empty()) {
            std::string fname = fi.filename;
            std::string filt = state.filter_text;
            std::transform(fname.begin(), fname.end(), fname.begin(), ::tolower);
            std::transform(filt.begin(), filt.end(), filt.begin(), ::tolower);
            if (fname.find(filt) == std::string::npos) continue;
        }

        state.filtered_indices.push_back(i);
    }

    // Clamp cursor
    if (state.filtered_indices.empty()) {
        state.cursor_pos = 0;
    } else if (state.cursor_pos >= state.filtered_indices.size()) {
        state.cursor_pos = state.filtered_indices.size() - 1;
    }
}

static void toggle_select(SelectState& state) {
    if (state.filtered_indices.empty()) return;
    auto idx = state.filtered_indices[state.cursor_pos];
    state.items[idx].selected = !state.items[idx].selected;
}

static void select_all(SelectState& state, bool value) {
    for (auto idx : state.filtered_indices) {
        state.items[idx].selected = value;
    }
}

static void invert_selection(SelectState& state) {
    for (auto idx : state.filtered_indices) {
        state.items[idx].selected = !state.items[idx].selected;
    }
}

static int count_selected(const SelectState& state) {
    int c = 0;
    for (const auto& item : state.items)
        if (item.selected) c++;
    return c;
}

static uint64_t total_selected_size(const SelectState& state) {
    uint64_t s = 0;
    for (const auto& item : state.items)
        if (item.selected) s += item.file.file_size;
    return s;
}

static uint64_t total_selected_projected(const SelectState& state) {
    uint64_t s = 0;
    for (const auto& item : state.items)
        if (item.selected) s += item.file.projected_size;
    return s;
}

// ── Render ───────────────────────────────────────────────────────────────────

static void render_ui(const SelectState& state) {
    term_clear();
    term_hide_cursor();

    int term_rows = 24;

    // Try to get terminal size
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        term_rows = ws.ws_row;
    }

    // ── Title ─────────────────────────────────────────────────────────────
    term_goto(1, 1);
    term_color("1;36");  // bold cyan
    std::cout << "  batchpress — Select Files to Process";
    term_reset_color();

    // ── Filter bar ────────────────────────────────────────────────────────
    term_goto(3, 1);
    term_color("90");
    std::cout << "  Filter: ";
    term_reset_color();
    if (!state.filter_text.empty()) {
        term_color("33");
        std::cout << state.filter_text;
        term_reset_color();
    } else {
        std::cout << "(type to filter)";
    }

    // ── Summary ───────────────────────────────────────────────────────────
    term_goto(4, 1);
    term_color("90");
    int sel = count_selected(state);
    int total = static_cast<int>(state.items.size());
    std::cout << "  " << sel << "/" << total << " selected  |  "
              << fmt_bytes(total_selected_size(state)) << " → "
              << fmt_bytes(total_selected_projected(state));
    term_reset_color();

    // ── Column headers ────────────────────────────────────────────────────
    int header_row = 6;
    term_goto(header_row, 1);
    term_color("1;37");  // bold white
    std::cout << "  [ ] File";
    term_goto(header_row, 50);
    std::cout << "Original Res";
    term_goto(header_row, 62);
    std::cout << "Output Res";
    term_goto(header_row, 76);
    std::cout << "Size (Orig → Proj)";
    term_goto(header_row, 96);
    std::cout << "Savings";
    term_goto(header_row, 108);
    std::cout << "Quality Est.";
    term_reset_color();

    // ── Separator line under headers ──────────────────────────────────────
    term_goto(header_row + 1, 1);
    term_color("90");  // dim grey
    std::cout << "  ─────────────────────────────────────────────────────────────────────────────────────────────────";
    term_reset_color();

    // ── File list ─────────────────────────────────────────────────────────
    int list_start = header_row + 2;
    int list_end = term_rows - 4;  // leave room for footer
    int visible = list_end - list_start + 1;
    if (visible < 1) visible = 1;

    // Scroll: keep cursor visible
    size_t scroll_offset = 0;
    if (state.cursor_pos >= (size_t)visible) {
        scroll_offset = state.cursor_pos - visible + 1;
    }

    for (int i = 0; i < visible; ++i) {
        int row = list_start + i;
        size_t item_idx = scroll_offset + i;

        term_goto(row, 1);
        term_clear_line();

        if (item_idx >= state.filtered_indices.size()) continue;

        size_t real_idx = state.filtered_indices[item_idx];
        const auto& fi = state.items[real_idx].file;
        bool is_sel = state.items[real_idx].selected;
        bool is_cursor = (item_idx == state.cursor_pos);

        // Cursor marker
        if (is_cursor) {
            term_color("1;33");  // bold yellow
            std::cout << " >";
        } else {
            std::cout << "  ";
        }
        term_reset_color();

        // Checkbox
        term_color(is_sel ? "32" : "90");  // green if selected, grey if not
        std::cout << (is_sel ? "[✓]" : "[ ]");
        term_reset_color();

        // Filename (full, never truncated)
        std::string fname = fi.filename;

        if (is_cursor) term_color("1;37");  // bold white for cursor

        term_goto(row, 6);
        std::cout << fname;

        // Original resolution
        term_goto(row, 50);
        std::cout << fi.width << "x" << fi.height;

        // Projected resolution
        term_goto(row, 62);
        auto get_proj = [](const batchpress::FileItem& f) -> std::pair<uint32_t, uint32_t> {
            if (f.type == batchpress::FileItem::Type::Image)
                return {f.image_info().projected_width, f.image_info().projected_height};
            return {f.video_info().projected_width, f.video_info().projected_height};
        };
        auto [pw, ph] = get_proj(fi);
        if (pw > 0 && ph > 0 && (pw != fi.width || ph != fi.height)) {
            term_color("33");  // yellow to indicate change
            std::cout << pw << "x" << ph;
            term_reset_color();
        } else {
            term_color("90");
            std::cout << "same";
            term_reset_color();
        }

        // Size: original → projected
        term_goto(row, 76);
        std::cout << fmt_bytes(fi.file_size);
        if (fi.projected_size > 0 && fi.projected_size != fi.file_size) {
            std::cout << " →";
            term_color("32");  // green for savings
            std::cout << fmt_bytes(fi.projected_size);
            term_reset_color();
        }

        // Savings
        term_goto(row, 96);
        if (fi.savings_pct > 0) {
            if (fi.savings_pct >= 60) term_color("32");       // green
            else if (fi.savings_pct >= 30) term_color("33");   // yellow
            else term_color("31");                              // red
        }
        std::cout << std::setw(5) << std::fixed << std::setprecision(0)
                  << fi.savings_pct << "%";
        term_reset_color();

        // Quality estimate
        term_goto(row, 108);
        auto get_quality = [](const batchpress::FileItem& f) -> batchpress::QualityEstimate {
            if (f.type == batchpress::FileItem::Type::Image)
                return f.image_info().quality;
            return f.video_info().quality;
        };
        auto q = get_quality(fi);
        int stars = batchpress::quality_stars(q);
        (void)batchpress::quality_label(q);  // available for future use

        // Color by quality
        if (stars >= 5) term_color("32");       // green
        else if (stars >= 4) term_color("36");   // cyan
        else if (stars >= 3) term_color("33");   // yellow
        else term_color("31");                    // red

        for (int s = 0; s < stars; ++s) std::cout << "★";
        for (int s = stars; s < 5; ++s) std::cout << "☆";
        term_reset_color();

        if (is_cursor) term_reset_color();
    }

    // ── Footer with controls ──────────────────────────────────────────────
    int footer_row = term_rows - 2;
    term_goto(footer_row, 1);
    term_color("90");
    std::cout << "  ↑↓/kj:navigate  Space:toggle  a:all  i:invert  Enter:process  q:quit";
    term_reset_color();

    term_goto(footer_row + 1, 1);
    std::cout << "\033[?25h";  // show cursor briefly at bottom
}

// ── Main UI loop ─────────────────────────────────────────────────────────────

SelectResult run_select_ui(
    const batchpress::FileScanReport& report,
    const batchpress::Config& /*img_cfg*/,
    const batchpress::VideoConfig& /*vid_cfg*/,
    const std::string& filter_type,
    double min_savings_pct)
{
    SelectResult result;

    // Build initial state
    SelectState state;

    // Filter items by type and min_savings
    for (const auto& fi : report.files) {
        // Type filter
        if (filter_type == "image" && fi.type != batchpress::FileItem::Type::Image)
            continue;
        if (filter_type == "video" && fi.type != batchpress::FileItem::Type::Video)
            continue;

        // Min savings filter
        if (fi.savings_pct < min_savings_pct)
            continue;

        SelectItem item;
        item.file = fi;
        item.selected = true;
        state.items.push_back(item);
    }

    if (state.items.empty()) {
        std::cout << "\nNo files match the current filters.\n";
        return result;
    }

    // Initial filter
    rebuild_filter(state);

    // Setup terminal
    enable_raw_mode();
    term_hide_cursor();
    render_ui(state);

    bool quit_and_process = false;
    bool quit_without_process = false;

    while (true) {
        char ch = 0;
        Key key = read_key(&ch);

        bool need_render = true;

        switch (key) {
            case Key::Up:
                if (state.cursor_pos > 0) state.cursor_pos--;
                break;

            case Key::Down:
                if (state.cursor_pos + 1 < state.filtered_indices.size())
                    state.cursor_pos++;
                break;

            case Key::Space:
                toggle_select(state);
                break;

            case Key::CharA:
                // Toggle all: if all selected, deselect all; otherwise select all
                if (count_selected(state) == (int)state.items.size())
                    select_all(state, false);
                else
                    select_all(state, true);
                break;

            case Key::CharI:
                invert_selection(state);
                break;

            case Key::Enter:
                quit_and_process = true;
                need_render = false;
                break;

            case Key::CharQ:
            case Key::Escape:
                quit_without_process = true;
                need_render = false;
                break;

            case Key::Char:
                // Add to filter
                if (ch >= 32 && ch < 127) {
                    state.filter_text += ch;
                    state.cursor_pos = 0;
                    rebuild_filter(state);
                }
                break;

            case Key::Backspace:
                if (!state.filter_text.empty()) {
                    state.filter_text.pop_back();
                    state.cursor_pos = 0;
                    rebuild_filter(state);
                }
                break;

            default:
                need_render = false;
                break;
        }

        if (quit_and_process || quit_without_process) break;

        if (need_render) render_ui(state);
    }

    // Restore terminal
    restore_terminal();
    term_show_cursor();
    term_clear();

    if (quit_and_process) {
        // Collect selected files
        for (const auto& item : state.items) {
            if (item.selected) {
                result.selected.push_back(item.file);
            }
        }
        result.proceed_with_processing = true;

        std::cout << "\n  Processing " << result.selected.size()
                  << " selected files...\n\n";
    } else {
        std::cout << "\n  Cancelled.\n";
    }

    return result;
}

} // namespace cli
