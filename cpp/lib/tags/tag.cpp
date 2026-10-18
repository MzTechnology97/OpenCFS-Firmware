// SPDX-License-Identifier: Apache-2.0
#include "cfs/tags/tag.hpp"

namespace cfs::tags {

void TagReadout::add_raw(uint8_t index, const uint8_t data[16]) {
    if (raw_count >= kMaxRaw) {
        flags |= kFlagTruncated;
        return;
    }
    Raw& r = raw[raw_count++];
    r.index = index;
    for (int i = 0; i < 16; ++i) r.data[i] = data[i];
}

void TagReadout::set_material(const char* text, size_t len) {
    size_t n = 0;
    // Trim leading/trailing spaces and NULs; keep printable ASCII only.
    while (len && (text[len - 1] == ' ' || text[len - 1] == '\0')) --len;
    size_t start = 0;
    while (start < len && text[start] == ' ') ++start;
    for (size_t i = start; i < len && n < sizeof material; ++i) {
        const char c = text[i];
        material[n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    for (; n < sizeof material; ++n) material[n] = '\0';
}

size_t encode_tag_info(uint8_t slot, const TagReadout& r, uint8_t* out, size_t cap) {
    const size_t n = kTagInfoHeader + 17u * r.raw_count;
    if (cap < n) return 0;
    auto put16 = [&](size_t off, uint16_t v) {
        out[off] = static_cast<uint8_t>(v);
        out[off + 1] = static_cast<uint8_t>(v >> 8);
    };
    for (size_t i = 0; i < n; ++i) out[i] = 0;
    out[0] = slot;
    out[1] = static_cast<uint8_t>(r.decoder);
    out[2] = r.flags;
    out[3] = r.card.uid_len;
    for (uint8_t i = 0; i < r.card.uid_len && i < 10; ++i) out[4 + i] = r.card.uid[i];
    out[14] = r.card.atqa[0];
    out[15] = r.card.atqa[1];
    out[16] = r.card.sak;
    for (size_t i = 0; i < sizeof r.material; ++i) out[17 + i] = static_cast<uint8_t>(r.material[i]);
    out[33] = static_cast<uint8_t>(r.rgba >> 24);  // R G B A
    out[34] = static_cast<uint8_t>(r.rgba >> 16);
    out[35] = static_cast<uint8_t>(r.rgba >> 8);
    out[36] = static_cast<uint8_t>(r.rgba);
    put16(37, r.weight_g);
    put16(39, r.length_m);
    put16(41, r.diameter_centi_mm);
    put16(43, r.nozzle_min_c);
    put16(45, r.nozzle_max_c);
    out[47] = r.vendor_hint;
    out[48] = r.raw_count;
    for (uint8_t e = 0; e < r.raw_count; ++e) {
        uint8_t* q = out + kTagInfoHeader + 17u * e;
        q[0] = r.raw[e].index;
        for (int i = 0; i < 16; ++i) q[1 + i] = r.raw[e].data[i];
    }
    return n;
}

int TagPipeline::cached(const CardInfo& card) const {
    for (const Route& r : routes_) {
        if (!r.decoder || r.uid_len != card.uid_len) continue;
        bool same = true;
        for (uint8_t i = 0; i < r.uid_len; ++i) same = same && r.uid[i] == card.uid[i];
        if (same) return r.decoder - 1;
    }
    return -1;
}

void TagPipeline::remember(const CardInfo& card, size_t index) {
    Route* slot = nullptr;
    for (Route& r : routes_) {
        if (r.decoder && r.uid_len == card.uid_len) {
            bool same = true;
            for (uint8_t i = 0; i < r.uid_len; ++i) same = same && r.uid[i] == card.uid[i];
            if (same) slot = &r;
        }
    }
    if (!slot) {
        slot = &routes_[next_route_];
        next_route_ = static_cast<uint8_t>((next_route_ + 1) % 8);
    }
    slot->uid_len = card.uid_len;
    for (uint8_t i = 0; i < card.uid_len; ++i) slot->uid[i] = card.uid[i];
    slot->decoder = static_cast<uint8_t>(index + 1);
}

bool TagPipeline::read(TagSession& session, TagReadout& out) {
    const CardInfo card = session.card();
    out = TagReadout{};
    out.card = card;
    bool used = false;   // the tag has seen a transaction: reselect before the next decoder
    bool lost = false;   // the tag left the field
    auto attempt = [&](size_t i) {
        TagDecoder* d = decoders_[i];
        if (lost || !d->matches(card)) return false;
        if (used && !session.reactivate()) {
            lost = true;
            return false;
        }
        used = true;
        TagReadout r;
        r.card = card;
        if (!d->read(session, r)) return false;
        r.decoder = d->id();
        out = r;
        remember(card, i);
        return true;
    };
    const int first = cached(card);
    if (first >= 0 && attempt(static_cast<size_t>(first))) return true;
    for (size_t i = 0; i < count_; ++i) {
        if (static_cast<int>(i) != first && attempt(i)) return true;
    }
    return false;
}

}  // namespace cfs::tags
