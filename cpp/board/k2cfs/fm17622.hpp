// SPDX-License-Identifier: Apache-2.0
// FM17622 13.56 MHz reader front-ends of the K2 CFS (docs/spec/10 section 5).
// Register sequences follow the stock driver (0x0800BDD4..0x0800C8E6). The driver
// only reads tags: it has no write path (stock never calls its card_write at
// 0x0800C70E either). Tag formats are decoded by the core pipeline
// (cfs/tags, docs/spec/11). NOT qualified on hardware.
#pragma once

#include <cstdint>

#include "board_io.hpp"
#include "cfs/hal.hpp"
#include "cfs/tags/tag.hpp"

namespace cfs::board {

using tags::CardInfo;

// One FM17622 on its own software I2C bus, 7-bit address 0x28.
class Fm17622 {
public:
    enum class Activate : uint8_t { Ok, NoTag, Collision, SelectFailed };

    Fm17622(SoftI2c& bus, Clock& clock) : bus_(bus), clock_(clock) {}
    bool soft_reset();  // Command = SoftReset, then Command must read 0x20
    void configure();   // stock analog and protocol set-up (0x0800C0E4)
    void antenna(bool on);
    Activate activate(CardInfo& card);  // WUPA, anticollision and select, cascade 1/2
    bool authenticate_a(uint8_t block, const uint8_t key[6], const uint8_t uid4[4]);
    bool read_block(uint8_t block, uint8_t out[16]);  // also four Type 2 pages
    void clear_crypto();                              // leave a Crypto1 session
    // CRC-framed exchange (ISO-DEP blocks), waiting at most `max_ms`.
    bool transceive(const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_cap, size_t& rx_len,
                    uint32_t max_ms);
    // Poll the completion flags instead of the stock fixed waits (unqualified).
    void set_fast(bool on) { fast_ = on; }

private:
    bool write_reg(uint8_t reg, uint8_t value);
    uint8_t read_reg(uint8_t reg);
    void modify_reg(uint8_t reg, uint8_t mask, bool set);
    void start(bool crc, const uint8_t* fifo, uint8_t len, uint8_t command, uint8_t framing);
    void wait(uint32_t max_ms);
    bool anticollision(uint8_t sel, uint8_t out[5]);
    bool select(uint8_t sel, const uint8_t cl[5], uint8_t& sak);

    SoftI2c& bus_;
    Clock& clock_;
    bool fast_ = false;
};

// One activated tag on one reader, for the core tag pipeline.
class Fm17622Session : public tags::TagSession {
public:
    Fm17622Session(Fm17622& chip, CardInfo& card) : chip_(chip), card_(card) {}
    const CardInfo& card() const override { return card_; }
    bool reactivate() override;
    bool auth_a(uint8_t block, const uint8_t key[6]) override {
        return chip_.authenticate_a(block, key, card_.uid);
    }
    bool read16(uint8_t index, uint8_t out[16]) override { return chip_.read_block(index, out); }
    bool transceive(const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_cap, size_t& rx_len) override {
        return chip_.transceive(tx, tx_len, rx, rx_cap, rx_len, 20);  // within the default FWT
    }

private:
    Fm17622& chip_;
    CardInfo& card_;
};

// The two readers of the box. Slot A/B use reader 0, C/D reader 1 (docs/spec/05
// section 4). Both TX drivers are switched on together, so a neighbor tag in the
// shared field shows up as a collision (Detect::Multiple), as in stock.
class Fm17622Rfid : public RfidReader {
public:
    Fm17622Rfid(SoftI2c& bus0, SoftI2c& bus1, Clock& clock, tags::TagPipeline& pipeline)
        : chip_{Fm17622(bus0, clock), Fm17622(bus1, clock)}, buses_{&bus0, &bus1}, pipeline_(pipeline) {}

    // Soft-reset each reader, up to 3 tries (0x0800BF74). Bit n set = reader n OK.
    uint8_t init();
    Detect detect(int slot) override;
    bool read_uid(int slot, TagUid& uid) override;
    // Runs the decoders; the stock 40-character record when one applies.
    bool read_record(int slot, TagRecord& rec) override;
    const tags::TagReadout* last_tag(int slot) override { return have_[slot] ? &last_[slot] : nullptr; }

private:
    Fm17622& chip(int slot) { return chip_[slot / 2]; }
    Fm17622::Activate open(int slot);  // reset, configure, antenna on, activate

    Fm17622 chip_[2];
    SoftI2c* buses_[2];
    tags::TagPipeline& pipeline_;
    CardInfo card_[4];
    bool present_[4] = {};
    tags::TagReadout last_[4];
    bool have_[4] = {};
};

}  // namespace cfs::board
