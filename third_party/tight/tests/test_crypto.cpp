#include "test_framework.hpp"

#include "crypto/crypto.hpp"
#include "tight/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

using namespace tight::tight_detail;
using tight::Bytes;

namespace {

Bytes hex_to_bytes(const char* hex) {
    Bytes out;
    std::size_t n = std::strlen(hex);
    for (std::size_t i = 0; i < n; i += 2) {
        auto nib = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
            return static_cast<std::uint8_t>(c - 'a' + 10);
        };
        out.push_back(static_cast<std::uint8_t>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

std::string to_hex(const std::array<std::uint8_t, 32>& a) {
    const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (auto b : a) {
        s.push_back(d[(b >> 4) & 0xF]);
        s.push_back(d[b & 0xF]);
    }
    return s;
}

} // namespace

TEST_CASE(sha256_empty) {
    auto h = sha256(nullptr, 0);
    CHECK_EQ(to_hex(h),
             "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE(sha256_abc) {
    const char* msg = "abc";
    auto h = sha256(reinterpret_cast<const std::uint8_t*>(msg), 3);
    CHECK_EQ(to_hex(h),
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE(sha256_two_block_message) {
    // 448 bits: requires two compression blocks (padding completes the first)
    const char* msg =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    auto h = sha256(reinterpret_cast<const std::uint8_t*>(msg), 56);
    CHECK_EQ(to_hex(h),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE(x25519_rfc7748_vector) {
    // RFC 7748 section 6.1
    auto alice_priv = hex_to_bytes(
        "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    auto bob_priv = hex_to_bytes(
        "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");

    std::array<std::uint8_t, 32> base{};
    base[0] = 9;

    std::array<std::uint8_t, 32> alice_pub, bob_pub;
    std::array<std::uint8_t, 32> a_priv, b_priv;
    std::copy(alice_priv.begin(), alice_priv.end(), a_priv.begin());
    std::copy(bob_priv.begin(), bob_priv.end(), b_priv.begin());

    CHECK(x25519(alice_pub, a_priv, base));
    CHECK(x25519(bob_pub, b_priv, base));
    CHECK_EQ(to_hex(alice_pub),
             "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    CHECK_EQ(to_hex(bob_pub),
             "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");

    std::array<std::uint8_t, 32> shared_a, shared_b;
    CHECK(x25519(shared_a, a_priv, bob_pub));
    CHECK(x25519(shared_b, b_priv, alice_pub));
    CHECK_EQ(to_hex(shared_a),
             "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    CHECK(shared_a == shared_b);
}

TEST_CASE(x25519_generate_and_agree) {
    auto a = x25519_generate();
    auto b = x25519_generate();
    std::array<std::uint8_t, 32> sa, sb;
    CHECK(x25519(sa, a.private_key, b.public_key));
    CHECK(x25519(sb, b.private_key, a.public_key));
    CHECK(sa == sb);
    CHECK(sa != (std::array<std::uint8_t, 32>{}));
}

TEST_CASE(x25519_rejects_low_order_point) {
    auto kp = x25519_generate();
    std::array<std::uint8_t, 32> zero{};
    std::array<std::uint8_t, 32> out;
    CHECK(!x25519(out, kp.private_key, zero));
}

TEST_CASE(hkdf_rfc5869_test_case_1) {
    // RFC 5869 Test Case 1, first 32 bytes of the 42-byte OKM
    Bytes ikm = hex_to_bytes(
        "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b");
    Bytes salt = hex_to_bytes("000102030405060708090a0b0c");
    std::string info = "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9";
    auto okm = hkdf_sha256(ikm.data(), ikm.size(), salt.data(), salt.size(), info);
    CHECK_EQ(to_hex(okm),
             "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf");
}

TEST_CASE(hkdf_without_salt) {
    Bytes ikm = {0x01, 0x02, 0x03, 0x04};
    auto okm = hkdf_sha256(ikm.data(), ikm.size(), nullptr, 0, "info");
    CHECK(okm != (std::array<std::uint8_t, 32>{}));
    auto okm2 = hkdf_sha256(ikm.data(), ikm.size(), nullptr, 0, "info");
    CHECK(okm == okm2);
}

TEST_CASE(gcm_known_tag_empty_plaintext) {
    // NIST SP 800-38D test case 13: AES-256, all-zero key/IV, no AAD/PT
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, kGcmNonceSize> nonce{};
    std::uint8_t tag[16];
    CHECK(aes256_gcm_encrypt(key, nonce, nullptr, 0, nullptr, 0, nullptr, tag));
    const std::uint8_t expected[16] = {0x53, 0x0f, 0x8a, 0xfb, 0xc7, 0x45, 0x36,
                                       0xb9, 0xa9, 0x63, 0xb4, 0xf1, 0xc4, 0xcb,
                                       0x73, 0x8b};
    CHECK(std::memcmp(tag, expected, 16) == 0);
}

TEST_CASE(gcm_roundtrip_with_aad) {
    std::array<std::uint8_t, 32> key;
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::uint8_t>(i * 3 + 1);
    std::array<std::uint8_t, kGcmNonceSize> nonce;
    for (std::size_t i = 0; i < nonce.size(); ++i)
        nonce[i] = static_cast<std::uint8_t>(i + 10);

    std::uint8_t aad[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    std::uint8_t pt[100];
    for (std::size_t i = 0; i < sizeof(pt); ++i) pt[i] = static_cast<std::uint8_t>(i);
    std::uint8_t ct[100], tag[16], back[100];

    CHECK(aes256_gcm_encrypt(key, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, tag));
    CHECK(aes256_gcm_decrypt(key, nonce, aad, sizeof(aad), ct, sizeof(ct), tag, back));
    CHECK(std::memcmp(pt, back, sizeof(pt)) == 0);
}

TEST_CASE(gcm_rejects_tampered_tag) {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, kGcmNonceSize> nonce{};
    std::uint8_t aad[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    std::uint8_t pt[32] = {};
    std::uint8_t ct[32], tag[16], back[32];
    CHECK(aes256_gcm_encrypt(key, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, tag));
    tag[0] ^= 0x01;
    CHECK(!aes256_gcm_decrypt(key, nonce, aad, sizeof(aad), ct, sizeof(ct), tag, back));
}

TEST_CASE(gcm_rejects_tampered_ciphertext) {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, kGcmNonceSize> nonce{};
    std::uint8_t pt[32] = {};
    std::uint8_t ct[32], tag[16], back[32];
    CHECK(aes256_gcm_encrypt(key, nonce, nullptr, 0, pt, sizeof(pt), ct, tag));
    ct[17] ^= 0x80;
    CHECK(!aes256_gcm_decrypt(key, nonce, nullptr, 0, ct, sizeof(ct), tag, back));
}

TEST_CASE(gcm_rejects_wrong_aad) {
    std::array<std::uint8_t, 32> key{};
    std::array<std::uint8_t, kGcmNonceSize> nonce{};
    std::uint8_t pt[16] = {};
    std::uint8_t aad[3] = {1, 2, 3};
    std::uint8_t other_aad[3] = {1, 2, 4};
    std::uint8_t ct[16], tag[16], back[16];
    CHECK(aes256_gcm_encrypt(key, nonce, aad, sizeof(aad), pt, sizeof(pt), ct, tag));
    CHECK(!aes256_gcm_decrypt(key, nonce, other_aad, sizeof(other_aad), ct, sizeof(ct), tag, back));
    CHECK(aes256_gcm_decrypt(key, nonce, aad, sizeof(aad), ct, sizeof(ct), tag, back));
}
