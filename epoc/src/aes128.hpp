// Minimal AES-128 ECB decryption.
//
// This replaces emokit's dependency on libmcrypt, which is unmaintained and
// effectively unobtainable on Windows. The dongle only ever needs ECB
// decryption of two 16-byte blocks per report, so the full generality of a
// crypto library is not required here.
//
// ECB is used because that is what the device does. It is not a defensible
// mode choice in general; we are reproducing a fixed wire format, not
// designing a protocol.

#ifndef XAVIER_EPOC_AES128_HPP
#define XAVIER_EPOC_AES128_HPP

#include <array>
#include <cstdint>

namespace epoc::detail {

class Aes128Ecb {
public:
    explicit Aes128Ecb(const std::array<std::uint8_t, 16>& key) noexcept;

    /// Decrypt exactly one 16-byte block. `in` and `out` may alias.
    void decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const noexcept;

    /// Verify the implementation against the FIPS-197 appendix C.1 vector.
    static bool self_test() noexcept;

private:
    // 11 round keys of 16 bytes.
    std::array<std::uint8_t, 176> round_keys_{};
};

}  // namespace epoc::detail

#endif  // XAVIER_EPOC_AES128_HPP
