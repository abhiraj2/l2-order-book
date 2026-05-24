#include "transport/pcap_replay.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace transport {

ITCHFileReader::ITCHFileReader(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "ITCHFileReader: cannot open '%s'\n", path.c_str());
        std::abort();
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        fprintf(stderr, "ITCHFileReader: empty or unreadable file '%s'\n", path.c_str());
        std::abort();
    }

    buf_.resize(static_cast<std::size_t>(size));
    const std::size_t nread = fread(buf_.data(), 1, buf_.size(), f);
    fclose(f);

    if (nread != buf_.size()) {
        fprintf(stderr, "ITCHFileReader: short read on '%s' (got %zu of %zu bytes)\n",
                path.c_str(), nread, buf_.size());
        std::abort();
    }
}

const uint8_t* ITCHFileReader::next_message(uint16_t& out_len) noexcept {
    if (pos_ + 2 > buf_.size()) return nullptr;

    const uint16_t len = static_cast<uint16_t>(
        (static_cast<uint16_t>(buf_[pos_]) << 8) | buf_[pos_ + 1]
    );
    pos_ += 2;

    if (len == 0) return nullptr;
    if (pos_ + len > buf_.size()) return nullptr;

    const uint8_t* msg = buf_.data() + pos_;
    pos_ += len;
    out_len = len;
    return msg;
}

} // namespace transport
