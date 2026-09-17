// xavier-tools: Emotiv EPOC support library.
//
// Ported from emokit (Copyright (c) 2010 Daeken and Skadge,
// Copyright (c) 2011-2012 OpenYou Organization, http://openyou.org),
// whose per-file notice is an ISC-style permission grant and whose project
// LICENSE is a public-domain dedication with a 3-clause-BSD fallback. Both
// are reproduced in this repository's LICENSE file.
//
// Differences from the emokit original are deliberate and documented at the
// point they occur; the wire protocol itself is unchanged.

#ifndef XAVIER_EPOC_EPOC_HPP
#define XAVIER_EPOC_EPOC_HPP

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace epoc {

/// Vendor id shared by all Emotiv USB dongles.
inline constexpr std::uint16_t kVendorId = 0x21a1;
/// Product id shared by all Emotiv USB dongles.
inline constexpr std::uint16_t kProductId = 0x0001;

inline constexpr std::size_t kChannelCount = 14;
/// The dongle emits fixed 32-byte HID input reports.
inline constexpr std::size_t kPacketSize = 32;
/// Reports arrive at a nominal 128 Hz.
inline constexpr double kNominalSampleRateHz = 128.0;

/// Volts-per-count for the EPOC's 14-bit sigma-delta ADC, expressed in
/// microvolts. Emotiv documents 0.51 uV/count for the consumer EPOC; this is
/// a nominal figure and is not individually calibrated per headset, so treat
/// microvolt output as indicative rather than metrologically exact.
inline constexpr double kMicrovoltsPerCount = 0.51;

/// The 14 electrodes, in the headset's conventional left-to-right ordering.
/// Note this is *not* the ordering used by the contact-quality subframe (see
/// protocol.cpp); the mapping between the two is handled internally.
enum class Channel : int {
    AF3 = 0, F7, F3, FC5, T7, P7, O1, O2, P8, T8, FC6, F4, F8, AF4
};

inline constexpr std::array<Channel, kChannelCount> kChannels = {
    Channel::AF3, Channel::F7, Channel::F3, Channel::FC5,
    Channel::T7,  Channel::P7, Channel::O1, Channel::O2,
    Channel::P8,  Channel::T8, Channel::FC6, Channel::F4,
    Channel::F8,  Channel::AF4
};

/// Electrode label, e.g. "AF3". Never null, valid for the program's lifetime.
std::string_view channel_name(Channel c) noexcept;

/// Headset class. This selects the crypto key layout and is normally detected
/// from the dongle's HID feature report; it can be forced via OpenOptions
/// when detection is unavailable or wrong.
enum class DeviceVariant { Consumer, Research };

std::string_view variant_name(DeviceVariant v) noexcept;

/// One decrypted 32-byte report: a single 128 Hz sample across all channels.
struct Frame {
    /// Sequence counter, 0..127. The dongle substitutes a battery reading for
    /// this byte once per second; when it does, `counter` is reported as 128.
    std::uint8_t counter = 0;
    /// True when this report carried a battery reading instead of a counter.
    bool is_battery_frame = false;

    /// Raw 14-bit ADC counts, indexed by `static_cast<int>(Channel)`.
    std::array<int, kChannelCount> counts{};

    /// Most recent contact-quality reading per channel. The dongle reports
    /// quality for one electrode per packet, so these are last-known values
    /// refreshed at 1-16 Hz, not per-sample measurements. Raw units: emokit
    /// treats >4000 as a good contact.
    std::array<int, kChannelCount> quality{};

    int gyro_x = 0;
    int gyro_y = 0;

    /// Battery charge, percent. Carried over from the last battery frame.
    std::uint8_t battery = 0;

    /// The decrypted report, for protocol debugging.
    std::array<std::uint8_t, kPacketSize> raw{};

    int count(Channel c) const noexcept {
        return counts[static_cast<std::size_t>(c)];
    }
    double microvolts(Channel c) const noexcept {
        return count(c) * kMicrovoltsPerCount;
    }
    int contact_quality(Channel c) const noexcept {
        return quality[static_cast<std::size_t>(c)];
    }
};

/// A dongle visible on the USB bus.
struct DeviceInfo {
    std::string path;    ///< Platform HID path, suitable for OpenOptions::path.
    std::string serial;  ///< Dongle serial; the decryption key is derived from it.
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    /// USB interface number, or -1 if the platform does not report one. The
    /// dongle presents *two* HID interfaces and only one of them carries EEG
    /// reports; the other opens successfully but never yields data. See
    /// streaming_candidates().
    int interface_number = -1;
};

/// Every HID interface of every connected Emotiv dongle, in bus order.
/// Returns an empty vector when none are present rather than throwing.
std::vector<DeviceInfo> enumerate();

/// The same set as enumerate(), reordered so the interface most likely to
/// carry EEG data comes first.
///
/// The dongle exposes two HID interfaces. Interface 1 is the one that streams;
/// interface 0 opens without error and then simply never delivers a report,
/// which is a confusing failure mode. emokit dealt with this by trying index 1
/// and falling back to index 0, and this function encodes the same preference
/// so callers can probe candidates in a sensible order.
std::vector<DeviceInfo> streaming_candidates();

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct OpenOptions {
    /// Open this exact HID path. Takes precedence over `index`.
    std::optional<std::string> path;
    /// Otherwise open the Nth matching dongle (default: the first).
    std::size_t index = 0;
    /// Use this serial for key derivation instead of the one the dongle
    /// reports. For recovering from dongles that report a mangled serial.
    std::optional<std::string> serial_override;
    /// Skip feature-report detection and assume this variant.
    std::optional<DeviceVariant> variant_override;
};

/// An open dongle. Move-only; closes on destruction.
class Device {
public:
    explicit Device(const OpenOptions& opts = OpenOptions{});
    ~Device();

    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    const std::string& serial() const noexcept;
    DeviceVariant variant() const noexcept;
    /// The derived AES-128 key, for debugging against known-good keys.
    const std::array<std::uint8_t, 16>& key() const noexcept;

    /// Read and decode one report, blocking up to `timeout`.
    /// Returns false on timeout; throws Error on transport failure.
    bool read_frame(Frame& out,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---------------------------------------------------------------------------
// Protocol primitives. Exposed so they can be unit-tested without hardware
// and reused by other tools in this repo.
// ---------------------------------------------------------------------------

/// Derive the AES-128 key from a dongle serial. Only the last four characters
/// of the serial participate. Throws Error if the serial is shorter than four
/// characters.
std::array<std::uint8_t, 16> derive_key(std::string_view serial, DeviceVariant variant);

/// Gather the 14 bits named by `bits` out of a decrypted packet into a 14-bit
/// sample value. `bits[i]` supplies bit i of the result, so `bits[0]` is the
/// least significant bit and `bits[13]` the most significant. Bit positions
/// are counted from packet byte 1, since byte 0 carries the counter.
int unpack_level(const std::uint8_t packet[kPacketSize], const std::uint8_t bits[kChannelCount]) noexcept;

/// Map a raw battery byte to a percentage of full charge.
std::uint8_t battery_percent(std::uint8_t raw) noexcept;

/// Decode an already-decrypted 32-byte packet into `out`. `out` is used as
/// carry-in for the sticky fields (contact quality, battery), so pass the
/// same Frame back in across a stream.
void decode_packet(const std::uint8_t packet[kPacketSize], Frame& out) noexcept;

}  // namespace epoc

#endif  // XAVIER_EPOC_EPOC_HPP
