#include "aes128.hpp"

#include <algorithm>

namespace epoc::detail {
namespace {

/// Multiply by x in GF(2^8) with the AES modulus 0x11b.
constexpr std::uint8_t xtime(std::uint8_t a) noexcept {
    return static_cast<std::uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

/// Multiply in GF(2^8).
constexpr std::uint8_t gmul(std::uint8_t a, std::uint8_t b) noexcept {
    std::uint8_t p = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

constexpr std::uint8_t rotl8(std::uint8_t x, int s) noexcept {
    return static_cast<std::uint8_t>((x << s) | (x >> (8 - s)));
}

// The S-box is *generated* rather than tabulated. Transcribing 512 bytes of
// lookup table by hand is the likeliest way to get an AES implementation
// subtly wrong; deriving it from the definition (multiplicative inverse in
// GF(2^8), then the AES affine transform) is self-checking and costs ~65k
// operations once, at first use.
struct Tables {
    std::array<std::uint8_t, 256> sbox{};
    std::array<std::uint8_t, 256> rsbox{};

    Tables() noexcept {
        std::array<std::uint8_t, 256> inv{};
        inv[0] = 0;  // 0 has no inverse; AES defines its image as 0.
        for (int a = 1; a < 256; ++a) {
            for (int b = 1; b < 256; ++b) {
                if (gmul(static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)) == 1) {
                    inv[static_cast<std::size_t>(a)] = static_cast<std::uint8_t>(b);
                    break;
                }
            }
        }
        for (int i = 0; i < 256; ++i) {
            const std::uint8_t x = inv[static_cast<std::size_t>(i)];
            const std::uint8_t s = static_cast<std::uint8_t>(
                x ^ rotl8(x, 1) ^ rotl8(x, 2) ^ rotl8(x, 3) ^ rotl8(x, 4) ^ 0x63);
            sbox[static_cast<std::size_t>(i)] = s;
            rsbox[s] = static_cast<std::uint8_t>(i);
        }
    }
};

const Tables& tables() noexcept {
    static const Tables t;
    return t;
}

// The AES state is column-major: state[row][col] lives at flat index 4*col+row.

void inv_shift_rows(std::uint8_t s[16]) noexcept {
    std::uint8_t t;
    // Row 1 rotates right by 1.
    t = s[13]; s[13] = s[9]; s[9] = s[5]; s[5] = s[1]; s[1] = t;
    // Row 2 rotates right by 2, i.e. two swaps.
    t = s[2];  s[2] = s[10];  s[10] = t;
    t = s[6];  s[6] = s[14];  s[14] = t;
    // Row 3 rotates right by 3, i.e. left by 1.
    t = s[3];  s[3] = s[7];  s[7] = s[11]; s[11] = s[15]; s[15] = t;
}

void inv_mix_columns(std::uint8_t s[16]) noexcept {
    for (int c = 0; c < 4; ++c) {
        std::uint8_t* p = s + 4 * c;
        const std::uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
        p[0] = static_cast<std::uint8_t>(gmul(a0, 0x0e) ^ gmul(a1, 0x0b) ^ gmul(a2, 0x0d) ^ gmul(a3, 0x09));
        p[1] = static_cast<std::uint8_t>(gmul(a0, 0x09) ^ gmul(a1, 0x0e) ^ gmul(a2, 0x0b) ^ gmul(a3, 0x0d));
        p[2] = static_cast<std::uint8_t>(gmul(a0, 0x0d) ^ gmul(a1, 0x09) ^ gmul(a2, 0x0e) ^ gmul(a3, 0x0b));
        p[3] = static_cast<std::uint8_t>(gmul(a0, 0x0b) ^ gmul(a1, 0x0d) ^ gmul(a2, 0x09) ^ gmul(a3, 0x0e));
    }
}

}  // namespace

Aes128Ecb::Aes128Ecb(const std::array<std::uint8_t, 16>& key) noexcept {
    const auto& sbox = tables().sbox;
    std::copy(key.begin(), key.end(), round_keys_.begin());

    std::uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i) {
        std::uint8_t t[4] = {
            round_keys_[static_cast<std::size_t>((i - 1) * 4 + 0)],
            round_keys_[static_cast<std::size_t>((i - 1) * 4 + 1)],
            round_keys_[static_cast<std::size_t>((i - 1) * 4 + 2)],
            round_keys_[static_cast<std::size_t>((i - 1) * 4 + 3)],
        };
        if (i % 4 == 0) {
            // RotWord, then SubWord, then xor the round constant.
            const std::uint8_t a0 = t[0];
            t[0] = static_cast<std::uint8_t>(sbox[t[1]] ^ rcon);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[a0];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j) {
            round_keys_[static_cast<std::size_t>(i * 4 + j)] = static_cast<std::uint8_t>(
                round_keys_[static_cast<std::size_t>((i - 4) * 4 + j)] ^ t[j]);
        }
    }
}

void Aes128Ecb::decrypt_block(const std::uint8_t in[16], std::uint8_t out[16]) const noexcept {
    const auto& rsbox = tables().rsbox;

    std::uint8_t s[16];
    for (int i = 0; i < 16; ++i) {
        s[i] = static_cast<std::uint8_t>(in[i] ^ round_keys_[static_cast<std::size_t>(160 + i)]);
    }

    for (int round = 9; round >= 1; --round) {
        inv_shift_rows(s);
        for (int i = 0; i < 16; ++i) s[i] = rsbox[s[i]];
        for (int i = 0; i < 16; ++i) {
            s[i] = static_cast<std::uint8_t>(s[i] ^ round_keys_[static_cast<std::size_t>(round * 16 + i)]);
        }
        inv_mix_columns(s);
    }

    inv_shift_rows(s);
    for (int i = 0; i < 16; ++i) s[i] = rsbox[s[i]];
    for (int i = 0; i < 16; ++i) {
        out[i] = static_cast<std::uint8_t>(s[i] ^ round_keys_[static_cast<std::size_t>(i)]);
    }
}

bool Aes128Ecb::self_test() noexcept {
    // FIPS-197, appendix C.1.
    const std::array<std::uint8_t, 16> key = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const std::uint8_t cipher[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    const std::uint8_t expected[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

    std::uint8_t got[16];
    Aes128Ecb(key).decrypt_block(cipher, got);
    return std::equal(std::begin(got), std::end(got), std::begin(expected));
}

}  // namespace epoc::detail
