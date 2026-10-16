// SPDX-License-Identifier: Apache-2.0
// CFS RS-485 protocol constants (docs/03-rs485-protocol.md).
#pragma once

#include <cstddef>
#include <cstdint>

namespace cfs {

constexpr uint8_t kSync = 0xF7;
constexpr uint8_t kAddrSilentBroadcast = 0xEA;  // executed, never answered
constexpr uint8_t kAddrBoxBroadcast = 0xEB;     // answered with 0xEB as address
constexpr uint8_t kHeaderBoxRequest = 0xFF;
constexpr size_t kMaxPayload = 252;
constexpr uint32_t kInterByteTimeoutMs = 120;
constexpr int kSlotCount = 4;

enum class Cmd : uint8_t {
    KeepAlive = 0x01,
    RfidRecords = 0x02,
    RfidRemaining = 0x03,
    Tracking = 0x04,
    Buffer = 0x05,
    ManualFeeder = 0x06,
    ManualHub = 0x07,
    SensorMask = 0x08,
    SetManualSpeed = 0x09,
    BoxState = 0x0A,
    SetIndicator = 0x0B,
    GetMode = 0x0C,
    RfidControl = 0x0D,
    Odometer = 0x0E,
    Option0F = 0x0F,
    Load = 0x10,
    Unload = 0x11,
    FeedDistance = 0x12,
    FeedToHub = 0x13,
    Version = 0x14,
    ModuleStatus = 0x15,
    Output30 = 0x30,
    SetMode = 0x50,
    BoxIndex = 0x51,
    GetEnvironment = 0x52,
    EnvironmentReply = 0x53,  // reply opcode of 0x52, and host->box display values
    Echo = 0x55,
    Reboot = 0x56,
    UniqueId = 0x5B,
    SecureChallenge = 0x5C,
    FactoryTest = 0x5D,
    RuntimeConfig = 0x57,  // K2-OpenHost Runtime Config API v2 sub-commands (docs/12)
    TagInfo = 0x60,  // OpenCFS extension: last tag readout of a slot (docs/11)
};

enum class Status : uint8_t {
    Ok = 0x00,
    InvalidParam = 0x01,
    BadCrc = 0x02,
    Busy = 0x03,
    Stage0SensorTimeout = 0x05,
    BufferFillTimeout = 0x06,
    SlotEmpty = 0x08,
    Stage0OdometerTimeout = 0x09,
    FeedTimeout = 0x0A,
    Overtravel = 0x0B,
    OdometerStalled = 0x0C,
    BufferNotFull = 0x0D,
    Stage7NoMotion = 0x0E,
    UnloadBufferTimeout = 0x13,
    UnloadHubPeTimeout = 0x14,
    UnloadNoFilament = 0x16,
    UnloadInletClear = 0x17,
    UnloadAllEmpty = 0x18,
    UnloadMotorBlocked = 0x19,
    UnloadOdometerTimeout = 0x1A,
    SlotEvent = 0x30,
    Runout = 0x50,
    BufferRefillStalled = 0x51,
    BufferRefillNoMotion = 0x52,
    PrintFeedError = 0x53,
};

enum class BoxState : uint8_t { Idle = 0, Preload = 1, Print = 2, Reload = 3, Error = 4, Test = 5 };

enum class SlotEvent : uint8_t { None = 0, Inserted = 1, Removed = 2, Redetected = 3 };

// Buffer switch state: (!full_switch) | (!empty_switch) << 1.
enum class BufferState : uint8_t { Between = 0, Full = 1, Empty = 2, Fault = 3 };

constexpr uint8_t to_u8(Status s) { return static_cast<uint8_t>(s); }
constexpr uint8_t to_u8(Cmd c) { return static_cast<uint8_t>(c); }

}  // namespace cfs
