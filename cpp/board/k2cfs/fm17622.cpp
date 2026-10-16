// SPDX-License-Identifier: Apache-2.0
#include "fm17622.hpp"

namespace cfs::board {

namespace {
constexpr uint8_t kAddr = 0x28;

// Registers (MFRC522-compatible map).
enum : uint8_t {
    kCommand = 0x01, kComIrq = 0x04, kStatus2 = 0x08, kFifoData = 0x09, kFifoLevel = 0x0A, kControl = 0x0C,
    kBitFraming = 0x0D, kTxMode = 0x12, kRxMode = 0x13, kTxControl = 0x14, kTxAsk = 0x15,
    kRxThreshold = 0x18, kRfCfg = 0x26, kGsN = 0x27, kCwGsP = 0x28, kModGsP = 0x29,
    kVendor36 = 0x36,
};
// Commands.
enum : uint8_t { kIdle = 0x00, kTransceive = 0x0C, kMfAuthent = 0x0E, kSoftReset = 0x0F };

constexpr uint8_t kFlushFifo = 0x80;
constexpr uint8_t kRxIrq = 0x20, kIdleIrq = 0x10, kErrIrq = 0x02;  // ComIrq bits
constexpr size_t kFifoSize = 64;
constexpr uint8_t kStartSend = 0x80;
constexpr uint8_t kCrcOn = 0x80;          // TxMode / RxMode CRC enable
constexpr uint8_t kCrypto1On = 0x08;      // Status2
constexpr uint8_t kTx1Tx2 = 0x03;         // TxControl: both antenna drivers

constexpr uint8_t kWupa = 0x52, kSelCl1 = 0x93, kSelCl2 = 0x95, kSelCl3 = 0x97, kCascadeTag = 0x88;
constexpr uint8_t kMfAuthKeyA = 0x60, kMfRead = 0x30;

}  // namespace

// ---- one reader -------------------------------------------------------------------
bool Fm17622::write_reg(uint8_t reg, uint8_t value) {
    const uint8_t b[2] = {static_cast<uint8_t>(reg & 0x3F), value};
    return bus_.write(kAddr, b, 2);
}

uint8_t Fm17622::read_reg(uint8_t reg) {
    const uint8_t r = reg & 0x3F;
    uint8_t v = 0;
    if (bus_.write(kAddr, &r, 1)) bus_.read(kAddr, &v, 1);
    return v;
}

void Fm17622::modify_reg(uint8_t reg, uint8_t mask, bool set) {
    const uint8_t v = read_reg(reg);
    write_reg(reg, set ? static_cast<uint8_t>(v | mask) : static_cast<uint8_t>(v & ~mask));
}

bool Fm17622::soft_reset() {
    write_reg(kCommand, kSoftReset);
    clock_.sleep_ms(2);
    return read_reg(kCommand) == 0x20;
}

void Fm17622::configure() {
    modify_reg(kControl, 0x10, true);
    modify_reg(kTxAsk, 0x40, true);  // 100 % ASK
    write_reg(kTxMode, 0x00);
    write_reg(kRxMode, 0x00);
    write_reg(kGsN, 0xF1);
    write_reg(kCwGsP, 0x3F);
    write_reg(kModGsP, 0x01);
    write_reg(kRfCfg, 0x58);
    write_reg(kRxThreshold, 0x84);
    write_reg(kVendor36, 0x40);
}

void Fm17622::antenna(bool on) {
    modify_reg(kTxControl, kTx1Tx2, on);
    clock_.sleep_ms(10);
}

// Stock frame start: CRC mode, idle, flush, FIFO bytes, command, BitFraming.
void Fm17622::start(bool crc, const uint8_t* fifo, uint8_t len, uint8_t command, uint8_t framing) {
    write_reg(kTxMode, crc ? kCrcOn : 0x00);
    write_reg(kRxMode, crc ? kCrcOn : 0x00);
    write_reg(kCommand, kIdle);
    write_reg(kComIrq, 0x7F);  // clear the completion flags for wait()
    write_reg(kFifoLevel, kFlushFifo);
    for (uint8_t i = 0; i < len; ++i) write_reg(kFifoData, fifo[i]);
    write_reg(kCommand, command);
    write_reg(kBitFraming, framing);
}

// Stock waits a fixed time after each command. With fast mode the driver polls
// ComIrq (RxIRq/IdleIRq/ErrIRq) every millisecond instead and keeps the stock time
// as the upper bound. Fast mode is not qualified on hardware: off by default.
void Fm17622::wait(uint32_t max_ms) {
    if (!fast_) {
        clock_.sleep_ms(max_ms);
        return;
    }
    for (uint32_t t = 0; t < max_ms; ++t) {
        if (read_reg(kComIrq) & (kRxIrq | kIdleIrq | kErrIrq)) return;
        clock_.sleep_ms(1);
    }
}

bool Fm17622::transceive(const uint8_t* tx, size_t tx_len, uint8_t* rx, size_t rx_cap, size_t& rx_len,
                         uint32_t max_ms) {
    rx_len = 0;
    if (tx_len > kFifoSize) return false;
    start(true, tx, static_cast<uint8_t>(tx_len), kTransceive, kStartSend);
    wait(max_ms);
    const uint8_t n = read_reg(kFifoLevel);
    if (n == 0 || n > rx_cap) return false;
    for (uint8_t i = 0; i < n; ++i) rx[i] = read_reg(kFifoData);
    rx_len = n;
    return true;
}

bool Fm17622::anticollision(uint8_t sel, uint8_t out[5]) {
    const uint8_t cmd[2] = {sel, 0x20};
    start(false, cmd, 2, kTransceive, kStartSend);
    wait(sel == kSelCl1 ? 2 : 10);
    if (read_reg(kFifoLevel) != 5) return false;
    for (int i = 0; i < 5; ++i) out[i] = read_reg(kFifoData);
    return static_cast<uint8_t>(out[0] ^ out[1] ^ out[2] ^ out[3]) == out[4];  // BCC
}

bool Fm17622::select(uint8_t sel, const uint8_t cl[5], uint8_t& sak) {
    const uint8_t cmd[7] = {sel, 0x70, cl[0], cl[1], cl[2], cl[3], cl[4]};
    start(true, cmd, 7, kTransceive, kStartSend);
    wait(2);
    if (read_reg(kFifoLevel) != 1) return false;
    sak = read_reg(kFifoData);
    return true;
}

Fm17622::Activate Fm17622::activate(CardInfo& card) {
    card = CardInfo{};
    const uint8_t wupa = kWupa;
    start(false, &wupa, 1, kTransceive, kStartSend | 7);  // short frame, 7 bits
    wait(1);
    if (read_reg(kFifoLevel) != 2) return Activate::NoTag;
    card.atqa[0] = read_reg(kFifoData);
    card.atqa[1] = read_reg(kFifoData);

    // Cascade levels 1..3 (4, 7 or 10-byte UID). Stock stops after CL1 for ATQA
    // 04 00 and otherwise does CL2; the next level follows SAK bit 2 (UID not
    // complete), as ISO/IEC 14443-3 defines it.
    static const uint8_t kSel[3] = {kSelCl1, kSelCl2, kSelCl3};
    const bool single = card.atqa[0] == 0x04 && card.atqa[1] == 0x00;
    for (int level = 0; level < 3; ++level) {
        uint8_t cl[5];
        if (!anticollision(kSel[level], cl)) return Activate::Collision;
        if (!select(kSel[level], cl, card.sak)) return Activate::SelectFailed;
        const bool more = !single && (card.sak & 0x04) != 0 && cl[0] == kCascadeTag && level < 2;
        const int first = more ? 1 : 0;
        for (int i = first; i < 4; ++i) card.uid[card.uid_len++] = cl[i];
        if (!more) return Activate::Ok;
    }
    return Activate::SelectFailed;
}

bool Fm17622::authenticate_a(uint8_t block, const uint8_t key[6], const uint8_t uid4[4]) {
    write_reg(kStatus2, 0x00);  // drop any previous Crypto1 session
    const uint8_t cmd[12] = {kMfAuthKeyA, block, key[0], key[1], key[2], key[3], key[4],
                             key[5], uid4[0], uid4[1], uid4[2], uid4[3]};
    start(true, cmd, 12, kMfAuthent, kStartSend);
    wait(10);
    return (read_reg(kStatus2) & kCrypto1On) != 0;
}

void Fm17622::clear_crypto() { write_reg(kStatus2, 0x00); }

bool Fm17622Session::reactivate() {
    chip_.clear_crypto();
    CardInfo again;
    if (chip_.activate(again) != Fm17622::Activate::Ok || again.uid_len != card_.uid_len) return false;
    for (uint8_t i = 0; i < card_.uid_len; ++i) {
        if (again.uid[i] != card_.uid[i]) return false;  // another tag took its place
    }
    return true;
}

bool Fm17622::read_block(uint8_t block, uint8_t out[16]) {
    if (block > 0x3F) return false;
    const uint8_t cmd[2] = {kMfRead, block};
    start(true, cmd, 2, kTransceive, kStartSend);
    wait(10);
    if (read_reg(kFifoLevel) != 16) return false;
    for (int i = 0; i < 16; ++i) out[i] = read_reg(kFifoData);
    return true;
}

// ---- both readers -------------------------------------------------------------------
uint8_t Fm17622Rfid::init() {
    uint8_t ok = 0;
    for (int r = 0; r < 2; ++r) {
        buses_[r]->init();
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (chip_[r].soft_reset()) {
                ok = static_cast<uint8_t>(ok | (1u << r));
                break;
            }
        }
    }
    return ok;
}

Fm17622::Activate Fm17622Rfid::open(int slot) {
    Fm17622& c = chip(slot);
    c.soft_reset();
    c.configure();
    c.antenna(true);
    const Fm17622::Activate a = c.activate(card_[slot]);
    present_[slot] = a == Fm17622::Activate::Ok;
    if (!present_[slot]) c.antenna(false);
    return a;
}

RfidReader::Detect Fm17622Rfid::detect(int slot) {
    switch (open(slot)) {
        case Fm17622::Activate::Ok:
            return Detect::One;
        case Fm17622::Activate::Collision:
            return Detect::Multiple;  // stock code 2: rotate the neighbor away
        default:
            return Detect::None;
    }
}

bool Fm17622Rfid::read_uid(int slot, TagUid& uid) {
    if (open(slot) != Fm17622::Activate::Ok) return false;
    uid.len = card_[slot].uid_len;
    for (uint8_t i = 0; i < uid.len; ++i) uid.bytes[i] = card_[slot].uid[i];
    return true;
}

bool Fm17622Rfid::read_record(int slot, TagRecord& rec) {
    if (!present_[slot] && open(slot) != Fm17622::Activate::Ok) return false;
    Fm17622Session session(chip(slot), card_[slot]);
    pipeline_.read(session, last_[slot]);
    have_[slot] = true;
    chip(slot).antenna(false);
    present_[slot] = false;
    if (!last_[slot].has_creality) return false;
    rec = last_[slot].creality;
    return true;
}

}  // namespace cfs::board
