// HID transport for the Emotiv EPOC USB dongle, via hidapi.
//
// hidapi abstracts the three platform backends we care about: the Windows HID
// API, Linux hidraw, and macOS IOKit. Notably, reports on this device carry no
// report id, and hidapi already normalises away the leading zero byte that
// Windows prepends, so a 32-byte read is correct on every platform.

#include "epoc/epoc.hpp"

#include "aes128.hpp"

#include <hidapi.h>

#include <algorithm>
#include <mutex>
#include <string>

namespace epoc {
namespace {

/// The dongle reports whether it is a consumer or research unit in a 9-byte
/// HID feature report. A consumer unit answers with exactly this.
constexpr std::size_t kFeatureReportSize = 9;
constexpr std::uint8_t kFeatureReportId = 0;
constexpr std::uint8_t kConsumerFeatureReport[kFeatureReportSize] = {
    0x00, 0xa0, 0xff, 0x1f, 0xff, 0x00, 0x00, 0x00, 0x00};

/// hid_init/hid_exit are global and not reference counted by hidapi itself, so
/// we count them here; this keeps multiple concurrent Devices safe.
class HidLibrary {
public:
    static void acquire() {
        std::lock_guard<std::mutex> lock(mutex());
        if (refcount()++ == 0) {
            if (hid_init() != 0) {
                refcount()--;
                throw Error("hid_init() failed: could not initialise the HID backend");
            }
        }
    }
    static void release() noexcept {
        std::lock_guard<std::mutex> lock(mutex());
        if (refcount() > 0 && --refcount() == 0) {
            hid_exit();
        }
    }

private:
    static std::mutex& mutex() {
        static std::mutex m;
        return m;
    }
    static int& refcount() {
        static int n = 0;
        return n;
    }
};

/// RAII wrapper so an exception between acquire and use cannot leak the init.
class HidGuard {
public:
    HidGuard() { HidLibrary::acquire(); }
    ~HidGuard() { HidLibrary::release(); }
    HidGuard(const HidGuard&) = delete;
    HidGuard& operator=(const HidGuard&) = delete;
};

/// Dongle serials are ASCII. Narrow explicitly rather than casting each
/// wchar_t to a byte: wchar_t is 16-bit on Windows but 32-bit on Linux and
/// macOS, so emokit's cast was only ever correct on Windows.
std::string narrow(const wchar_t* w) {
    std::string out;
    if (!w) return out;
    for (const wchar_t* p = w; *p; ++p) {
        const auto c = static_cast<std::uint32_t>(*p);
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

std::string hid_error_message(hid_device* dev) {
    const wchar_t* msg = hid_error(dev);
    const std::string narrowed = narrow(msg);
    return narrowed.empty() ? std::string("unknown HID error") : narrowed;
}

}  // namespace

std::vector<DeviceInfo> enumerate() {
    HidGuard guard;
    std::vector<DeviceInfo> found;

    hid_device_info* list = hid_enumerate(kVendorId, kProductId);
    for (hid_device_info* cur = list; cur; cur = cur->next) {
        DeviceInfo info;
        info.path = cur->path ? cur->path : "";
        info.serial = narrow(cur->serial_number);
        info.vendor_id = cur->vendor_id;
        info.product_id = cur->product_id;
        info.interface_number = cur->interface_number;
        found.push_back(std::move(info));
    }
    hid_free_enumeration(list);
    return found;
}

std::vector<DeviceInfo> streaming_candidates() {
    auto devices = enumerate();
    // Interface 1 first, everything else after, otherwise preserving bus
    // order. Stable so that multiple dongles keep a predictable ordering.
    std::stable_sort(devices.begin(), devices.end(),
                     [](const DeviceInfo& a, const DeviceInfo& b) {
                         const int rank_a = (a.interface_number == 1) ? 0 : 1;
                         const int rank_b = (b.interface_number == 1) ? 0 : 1;
                         return rank_a < rank_b;
                     });
    return devices;
}

struct Device::Impl {
    HidGuard guard;
    hid_device* handle = nullptr;
    std::string serial;
    DeviceVariant variant = DeviceVariant::Consumer;
    bool variant_detected = false;
    std::array<std::uint8_t, 16> key{};
    std::unique_ptr<detail::Aes128Ecb> cipher;

    ~Impl() {
        if (handle) hid_close(handle);
    }
};

namespace {

/// Probe the feature report. Returns false if the device would not answer, in
/// which case the caller falls back to the consumer layout (as emokit did,
/// albeit by accident through integer conversion).
bool detect_variant(hid_device* handle, DeviceVariant& out) {
    unsigned char buf[kFeatureReportSize] = {};
    buf[0] = kFeatureReportId;
    const int n = hid_get_feature_report(handle, buf, sizeof(buf));
    if (n != static_cast<int>(kFeatureReportSize)) {
        return false;
    }
    // Anything that is not byte-for-byte the consumer report is assumed to be
    // a research unit.
    out = DeviceVariant::Consumer;
    for (std::size_t i = 0; i < kFeatureReportSize; ++i) {
        if (buf[i] != kConsumerFeatureReport[i]) {
            out = DeviceVariant::Research;
            break;
        }
    }
    return true;
}

}  // namespace

Device::Device(const OpenOptions& opts) : impl_(std::make_unique<Impl>()) {
    if (!detail::Aes128Ecb::self_test()) {
        // Refuse to hand back plausible-looking garbage.
        throw Error("internal AES-128 self test failed; refusing to decode");
    }

    std::string chosen_path;
    if (opts.path) {
        chosen_path = *opts.path;
    } else {
        const auto devices = enumerate();
        if (devices.empty()) {
            throw Error("no Emotiv dongle found (looked for USB " +
                        std::string("21a1:0001") + "); is the receiver plugged in?");
        }
        if (opts.index >= devices.size()) {
            throw Error("device index " + std::to_string(opts.index) + " is out of range; " +
                        std::to_string(devices.size()) + " dongle(s) present");
        }
        chosen_path = devices[opts.index].path;
    }

    impl_->handle = hid_open_path(chosen_path.c_str());
    if (!impl_->handle) {
        throw Error("could not open HID path '" + chosen_path +
                    "'. On Linux this is usually a permissions problem -- see "
                    "the udev rule in packaging/. On Windows, close any other "
                    "software holding the dongle.");
    }

    if (opts.variant_override) {
        impl_->variant = *opts.variant_override;
        impl_->variant_detected = true;
    } else {
        impl_->variant_detected = detect_variant(impl_->handle, impl_->variant);
    }

    if (opts.serial_override) {
        impl_->serial = *opts.serial_override;
    } else {
        wchar_t wide[256] = {};
        if (hid_get_serial_number_string(impl_->handle, wide,
                                         sizeof(wide) / sizeof(wide[0])) != 0) {
            throw Error("could not read the dongle serial number (" +
                        hid_error_message(impl_->handle) +
                        "); the decryption key is derived from it. You can "
                        "supply one explicitly with --serial.");
        }
        impl_->serial = narrow(wide);
    }

    impl_->key = derive_key(impl_->serial, impl_->variant);
    impl_->cipher = std::make_unique<detail::Aes128Ecb>(impl_->key);
}

Device::~Device() = default;
Device::Device(Device&&) noexcept = default;
Device& Device::operator=(Device&&) noexcept = default;

const std::string& Device::serial() const noexcept { return impl_->serial; }
DeviceVariant Device::variant() const noexcept { return impl_->variant; }
const std::array<std::uint8_t, 16>& Device::key() const noexcept { return impl_->key; }

bool Device::read_frame(Frame& out, std::chrono::milliseconds timeout) {
    unsigned char encrypted[kPacketSize] = {};

    const int n = hid_read_timeout(impl_->handle, encrypted, kPacketSize,
                                   static_cast<int>(timeout.count()));
    if (n < 0) {
        throw Error("read from dongle failed (" + hid_error_message(impl_->handle) +
                    "); was it unplugged?");
    }
    if (n == 0) {
        return false;  // timed out
    }
    if (n != static_cast<int>(kPacketSize)) {
        throw Error("short read from dongle: expected " + std::to_string(kPacketSize) +
                    " bytes, got " + std::to_string(n));
    }

    // The payload is two independently-ECB'd 16-byte blocks.
    std::uint8_t plain[kPacketSize];
    impl_->cipher->decrypt_block(encrypted, plain);
    impl_->cipher->decrypt_block(encrypted + 16, plain + 16);

    decode_packet(plain, out);
    return true;
}

}  // namespace epoc
