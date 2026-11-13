#pragma once
// Rewind history: a ring of recent save states kept as compact differences.
//
// The newest snapshot is kept whole. Every older one is stored as the bytes that changed between
// it and the snapshot after it (XOR, with runs of unchanged bytes skipped), so a second of play
// usually costs a small fraction of a full save state. Stepping back undoes the newest
// difference, which gives the snapshot before it. The oldest entries are dropped once the history
// holds more than max_entries snapshots or max_bytes of differences.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace supersaga::rewind {

namespace detail {

inline void put_varint(std::vector<std::uint8_t>& out, std::size_t v) {
    while (v >= 0x80) {
        out.push_back(std::uint8_t(v | 0x80));
        v >>= 7;
    }
    out.push_back(std::uint8_t(v));
}

inline bool get_varint(const std::uint8_t*& p, const std::uint8_t* end, std::size_t& v) {
    v = 0;
    for (int shift = 0; p < end && shift < 64; shift += 7) {
        const std::uint8_t b = *p++;
        v |= std::size_t(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
    }
    return false;
}

// Encodes a XOR b (same size) as pairs of (unchanged run, changed run + changed bytes).
inline std::vector<std::uint8_t> diff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::vector<std::uint8_t> out;
    const std::size_t n = a.size();
    std::size_t i = 0;
    while (i < n) {
        std::size_t same = i;
        // Skip unchanged bytes eight at a time where possible.
        while (same + 8 <= n) {
            std::uint64_t x, y;
            std::memcpy(&x, a.data() + same, 8);
            std::memcpy(&y, b.data() + same, 8);
            if (x != y) break;
            same += 8;
        }
        while (same < n && a[same] == b[same]) ++same;
        if (same == n) break;  // the rest is unchanged
        std::size_t changed = same;
        // A changed run ends at the first stretch of 8 unchanged bytes (shorter gaps are cheaper
        // to store as changed bytes than as a new pair).
        std::size_t quiet = 0;
        while (changed < n) {
            if (a[changed] == b[changed]) {
                if (++quiet == 8) { changed -= 7; break; }
            } else {
                quiet = 0;
            }
            ++changed;
        }
        if (changed == n) changed -= quiet;
        put_varint(out, same - i);
        put_varint(out, changed - same);
        for (std::size_t k = same; k < changed; ++k) out.push_back(std::uint8_t(a[k] ^ b[k]));
        i = changed;
    }
    return out;
}

// Applies a diff to data in place (XOR, so applying it turns either side into the other).
inline bool apply(std::vector<std::uint8_t>& data, const std::vector<std::uint8_t>& d) {
    const std::uint8_t* p = d.data();
    const std::uint8_t* end = p + d.size();
    std::size_t pos = 0;
    while (p < end) {
        std::size_t skip = 0, len = 0;
        if (!get_varint(p, end, skip) || !get_varint(p, end, len)) return false;
        pos += skip;
        if (pos + len > data.size() || std::size_t(end - p) < len) return false;
        for (std::size_t k = 0; k < len; ++k) data[pos + k] ^= p[k];
        p += len;
        pos += len;
    }
    return true;
}

}  // namespace detail

class History {
public:
    void set_limits(std::size_t max_entries, std::size_t max_bytes) {
        max_entries_ = max_entries;
        max_bytes_ = max_bytes;
        trim();
    }

    void clear() {
        diffs_.clear();
        newest_.clear();
        bytes_ = 0;
    }

    bool empty() const { return newest_.empty(); }
    // Snapshots that can still be stepped back to (not counting the newest).
    std::size_t depth() const { return diffs_.size(); }
    std::size_t bytes() const { return bytes_; }

    // Adds the newest snapshot. A snapshot of a different size starts a new history.
    void push(std::vector<std::uint8_t> state) {
        if (!newest_.empty() && newest_.size() == state.size() && max_entries_ > 0) {
            std::vector<std::uint8_t> d = detail::diff(newest_, state);
            bytes_ += d.size();
            diffs_.push_back(std::move(d));
        } else {
            diffs_.clear();
            bytes_ = 0;
        }
        newest_ = std::move(state);
        trim();
    }

    // Steps back one snapshot and returns it, or nullptr when there is no older one.
    const std::vector<std::uint8_t>* step_back() {
        if (diffs_.empty()) return nullptr;
        if (!detail::apply(newest_, diffs_.back())) {
            clear();
            return nullptr;
        }
        bytes_ -= diffs_.back().size();
        diffs_.pop_back();
        return &newest_;
    }

private:
    void trim() {
        while (!diffs_.empty() && (diffs_.size() > max_entries_ || bytes_ > max_bytes_)) {
            bytes_ -= diffs_.front().size();
            diffs_.pop_front();
        }
    }

    std::deque<std::vector<std::uint8_t>> diffs_;  // oldest first; back() leads to newest_'s predecessor
    std::vector<std::uint8_t> newest_;
    std::size_t bytes_ = 0;
    std::size_t max_entries_ = 900;
    std::size_t max_bytes_ = std::size_t(256) << 20;
};

}  // namespace supersaga::rewind
