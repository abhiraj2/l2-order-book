#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace transport {

// Reads a Nasdaq ITCH binary file in the length-prefixed format:
//   [2-byte big-endian message length][message bytes] ...
//
// The file is loaded fully into memory at construction for replay speed.
// Typical day file is 1–3 GB decompressed; caller is responsible for
// decompressing (.gz → raw binary) before opening.
// Aborts on I/O failure (file not found, short read) — caller must ensure
// the path exists.  Exceptions are disabled project-wide (-fno-exceptions).
class ITCHFileReader {
public:
    explicit ITCHFileReader(const std::string& path);

    // Returns a pointer to the next raw message and sets out_len.
    // Returns nullptr at EOF or on framing error.
    const uint8_t* next_message(uint16_t& out_len) noexcept;

    void reset() noexcept { pos_ = 0; }
    bool eof() const noexcept { return pos_ >= buf_.size(); }
    std::size_t bytes_remaining() const noexcept {
        return pos_ < buf_.size() ? buf_.size() - pos_ : 0;
    }

private:
    std::vector<uint8_t> buf_;
    std::size_t          pos_ = 0;
};

} // namespace transport
