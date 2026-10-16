// SPDX-License-Identifier: Apache-2.0
// FM17622 13.56 MHz reader front-ends of the K2 CFS (docs/spec/10 section 5).
// Register sequences follow the stock driver (0x0800BDD4..0x0800C8E6). The driver
// only reads tags: it has no write path, like stock in practice (card_write at
// 0x0800C70E is never called). NOT qualified on hardware.
#pragma once

#include <cstdint>

#include "board_io.hpp"
#include "cfs/hal.hpp"

namespace cfs::board {

// Creality tag secrets. Stock derives MIFARE Key A from the UID and decrypts
// blocks 4..6 with AES-128 (S-boxes at 0x0802BD89 / 0x0802BE89). OpenCFS does
// not ship those keys: an integrator provides them through this interface.
// Without it, MIFARE Classic tags report a UID but no record.
class TagCrypto {
public:
    virtual ~TagCrypto() = default;
    virtual bool key_a(const uint8_t uid4[4], uint8_t key[6]) = 0;
    virtual bool decrypt_block(uint8_t block[16]) = 0;
};

struct CardInfo {
    uint8_t atqa[2] = {};
    uint8_t uid[7] = {};
    uint8_t uid_len = 0;  // 4 or 7
    uint8_t sak = 0;
    bool mifare_classic_1k() const { return atqa[0] == 0x04 && atqa[1] == 0x00 && sak == 0x08; }
};

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
    bool read_block(uint8_t block, uint8_t out[16]);

private:
    bool write_reg(uint8_t reg, uint8_t value);
    uint8_t read_reg(uint8_t reg);
    void modify_reg(uint8_t reg, uint8_t mask, bool set);
    void start(bool crc, const uint8_t* fifo, uint8_t len, uint8_t command, uint8_t framing);
    bool anticollision(uint8_t sel, uint8_t out[5]);
    bool select(uint8_t sel, const uint8_t cl[5], uint8_t& sak);

    SoftI2c& bus_;
    Clock& clock_;
};

// The two readers of the box. Slot A/B use reader 0, C/D reader 1 (docs/spec/05
// section 4). Both TX drivers are switched on together, so a neighbor tag in the
// shared field shows up as a collision (Detect::Multiple), as in stock.
class Fm17622Rfid : public RfidReader {
public:
    Fm17622Rfid(SoftI2c& bus0, SoftI2c& bus1, Clock& clock, TagCrypto* crypto = nullptr)
        : chip_{Fm17622(bus0, clock), Fm17622(bus1, clock)}, buses_{&bus0, &bus1}, crypto_(crypto) {}

    // Soft-reset each reader, up to 3 tries (0x0800BF74). Bit n set = reader n OK.
    uint8_t init();
    void set_crypto(TagCrypto* crypto) { crypto_ = crypto; }
    Detect detect(int slot) override;
    bool read_uid(int slot, TagUid& uid) override;
    bool read_record(int slot, TagRecord& rec) override;

private:
    Fm17622& chip(int slot) { return chip_[slot / 2]; }
    Fm17622::Activate open(int slot);  // reset, configure, antenna on, activate

    Fm17622 chip_[2];
    SoftI2c* buses_[2];
    TagCrypto* crypto_;
    CardInfo card_[4];
    bool present_[4] = {};
};

}  // namespace cfs::board
