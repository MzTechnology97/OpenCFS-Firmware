// SPDX-License-Identifier: Apache-2.0
#include "fm17622.hpp"

namespace cfs::board {

namespace {
constexpr uint8_t kAddr = 0x28;

// Registers (MFRC522-compatible map).
enum : uint8_t {
    kCommand = 0x01, kStatus2 = 0x08, kFifoData = 0x09, kFifoLevel = 0x0A, kControl = 0x0C,
    kBitFraming = 0x0D, kTxMode = 0x12, kRxMode = 0x13, kTxControl = 0x14, kTxAsk = 0x15,
    kRxThreshold = 0x18, kRfCfg = 0x26, kGsN = 0x27, kCwGsP = 0x28, kModGsP = 0x29,
    kVendor36 = 0x36,
};
// Commands.
enum : uint8_t { kIdle = 0x00, kTransceive = 0x0C, kMfAuthent = 0x0E, kSoftReset = 0x0F };

constexpr uint8_t kFlushFifo = 0x80;
constexpr uint8_t kStartSend = 0x80;
constexpr uint8_t kCrcOn = 0x80;          // TxMode / RxMode CRC enable
constexpr uint8_t kCrypto1On = 0x08;      // Status2
constexpr uint8_t kTx1Tx2 = 0x03;         // TxControl: both antenna drivers

constexpr uint8_t kWupa = 0x52, kSelCl1 = 0x93, kSelCl2 = 0x95, kCascadeTag = 0x88;
constexpr uint8_t kMfAuthKeyA = 0x60, kMfRead = 0x30;

// Creality record: blocks 4, 5 and the first 8 bytes of 6 (0x0800D3DE).
constexpr uint8_t kRecordBlocks[3] = {4, 5, 6};
// Other tags: pages 4..8 read with two READs, first 20 bytes hex-encoded.
constexpr uint8_t kPageA = 4, kPageB = 8;
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
    write_reg(kFifoLevel, kFlushFifo);
    for (uint8_t i = 0; i < len; ++i) write_reg(kFifoData, fifo[i]);
    write_reg(kCommand, command);
    write_reg(kBitFraming, framing);
}

bool Fm17622::anticollision(uint8_t sel, uint8_t out[5]) {
    const uint8_t cmd[2] = {sel, 0x20};
    start(false, cmd, 2, kTransceive, kStartSend);
    clock_.sleep_ms(sel == kSelCl1 ? 2 : 10);
    if (read_reg(kFifoLevel) != 5) return false;
    for (int i = 0; i < 5; ++i) out[i] = read_reg(kFifoData);
    return static_cast<uint8_t>(out[0] ^ out[1] ^ out[2] ^ out[3]) == out[4];  // BCC
}

bool Fm17622::select(uint8_t sel, const uint8_t cl[5], uint8_t& sak) {
    const uint8_t cmd[7] = {sel, 0x70, cl[0], cl[1], cl[2], cl[3], cl[4]};
    start(true, cmd, 7, kTransceive, kStartSend);
    clock_.sleep_ms(2);
    if (read_reg(kFifoLevel) != 1) return false;
    sak = read_reg(kFifoData);
    return true;
}

Fm17622::Activate Fm17622::activate(CardInfo& card) {
    card = CardInfo{};
    const uint8_t wupa = kWupa;
    start(false, &wupa, 1, kTransceive, kStartSend | 7);  // short frame, 7 bits
    clock_.sleep_ms(1);
    if (read_reg(kFifoLevel) != 2) return Activate::NoTag;
    card.atqa[0] = read_reg(kFifoData);
    card.atqa[1] = read_reg(kFifoData);

    uint8_t cl1[5];
    if (!anticollision(kSelCl1, cl1)) return Activate::Collision;
    if (!select(kSelCl1, cl1, card.sak)) return Activate::SelectFailed;
    const bool single = card.atqa[0] == 0x04 && card.atqa[1] == 0x00;
    if (single || cl1[0] != kCascadeTag) {
        for (int i = 0; i < 4; ++i) card.uid[i] = cl1[i];
        card.uid_len = 4;
        return Activate::Ok;
    }
    uint8_t cl2[5];
    if (!anticollision(kSelCl2, cl2)) return Activate::Collision;
    if (!select(kSelCl2, cl2, card.sak)) return Activate::SelectFailed;
    for (int i = 0; i < 3; ++i) card.uid[i] = cl1[1 + i];
    for (int i = 0; i < 4; ++i) card.uid[3 + i] = cl2[i];
    card.uid_len = 7;
    return Activate::Ok;
}

bool Fm17622::authenticate_a(uint8_t block, const uint8_t key[6], const uint8_t uid4[4]) {
    write_reg(kStatus2, 0x00);  // drop any previous Crypto1 session
    const uint8_t cmd[12] = {kMfAuthKeyA, block, key[0], key[1], key[2], key[3], key[4],
                             key[5], uid4[0], uid4[1], uid4[2], uid4[3]};
    start(true, cmd, 12, kMfAuthent, kStartSend);
    clock_.sleep_ms(10);
    return (read_reg(kStatus2) & kCrypto1On) != 0;
}

bool Fm17622::read_block(uint8_t block, uint8_t out[16]) {
    if (block > 0x3F) return false;
    const uint8_t cmd[2] = {kMfRead, block};
    start(true, cmd, 2, kTransceive, kStartSend);
    clock_.sleep_ms(10);
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
    Fm17622& c = chip(slot);
    const CardInfo& card = card_[slot];
    bool ok = false;
    if (card.mifare_classic_1k()) {
        uint8_t key[6];
        // Blocks 4..6 share sector 1: one authentication on block 4, as stock.
        ok = crypto_ && crypto_->key_a(card.uid, key) &&
             c.authenticate_a(kRecordBlocks[0] & 0xFC, key, card.uid);
        size_t pos = 0;
        for (uint8_t b : kRecordBlocks) {
            uint8_t data[16];
            ok = ok && c.read_block(b, data) && crypto_->decrypt_block(data);
            for (size_t i = 0; ok && i < 16 && pos < kTagRecordLen; ++i) rec.text[pos++] = static_cast<char>(data[i]);
        }
    } else {
        uint8_t data[32];
        ok = c.read_block(kPageA, data) && c.read_block(kPageB, data + 16);
        static const char kHex[] = "0123456789ABCDEF";
        for (size_t i = 0; ok && i < kTagRecordLen / 2; ++i) {
            rec.text[2 * i] = kHex[data[i] >> 4];
            rec.text[2 * i + 1] = kHex[data[i] & 0x0F];
        }
    }
    c.antenna(false);
    present_[slot] = false;
    return ok;
}

}  // namespace cfs::board
