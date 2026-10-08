// SPDX-License-Identifier: Apache-2.0
//
// jsonrpc/framing.hpp — newline-delimited framing over a byte stream.
//
//   MCP and ACP over stdio send one JSON envelope per line. A read from a
//   pipe returns whatever bytes are there: half a line, or three and a half.
//   LineSplitter turns those reads into whole lines; the host feeds it bytes
//   and hands each line to Engine::receive.
//
//       LineSplitter s;
//       for (auto& line : s.feed(bytes)) engine.receive(line);
//       if (eof) if (auto last = s.finish()) engine.receive(*last);
//
//   A trailing '\r' is dropped (CRLF peers). Empty lines are skipped. A line
//   longer than the limit is dropped whole rather than buffered forever; the
//   count of dropped lines is reported so the host can log it.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jsonrpc {

class LineSplitter {
public:
    explicit LineSplitter(std::size_t max_line = 64u << 20) : max_line_(max_line) {}

    // The complete lines in `bytes` (with what was buffered before). A
    // partial last line stays buffered for the next feed.
    [[nodiscard]] std::vector<std::string> feed(std::string_view bytes) {
        std::vector<std::string> out;
        for (std::size_t i = 0; i < bytes.size();) {
            const auto nl = bytes.find('\n', i);
            const auto end = nl == std::string_view::npos ? bytes.size() : nl;
            const auto chunk = bytes.substr(i, end - i);
            if (!skipping_) {
                if (buf_.size() + chunk.size() > max_line_) {
                    buf_.clear();
                    skipping_ = true;
                    ++dropped_;
                } else {
                    buf_.append(chunk);
                }
            }
            if (nl == std::string_view::npos) break;
            if (!skipping_) push(out);
            skipping_ = false;
            i = nl + 1;
        }
        return out;
    }

    // At end of stream: the unterminated last line, if any.
    [[nodiscard]] std::optional<std::string> finish() {
        std::vector<std::string> out;
        if (!skipping_) push(out);
        skipping_ = false;
        if (out.empty()) return std::nullopt;
        return std::move(out.front());
    }

    [[nodiscard]] std::size_t dropped() const noexcept { return dropped_; }

private:
    void push(std::vector<std::string>& out) {
        if (!buf_.empty() && buf_.back() == '\r') buf_.pop_back();
        if (!buf_.empty()) out.push_back(std::move(buf_));
        buf_.clear();
    }

    std::string buf_;
    std::size_t max_line_;
    std::size_t dropped_  = 0;
    bool        skipping_ = false;
};

// One envelope as a line ready to write.
[[nodiscard]] inline std::string line(std::string frame) {
    frame.push_back('\n');
    return frame;
}

}  // namespace jsonrpc
