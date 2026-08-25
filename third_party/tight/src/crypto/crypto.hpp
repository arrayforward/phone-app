#pragma once

// 加密原语（纯 C++ 实现，零外部依赖）：
//   X25519        —— RFC 7748 ECDH 密钥交换（Curve25519  Montgomery 阶梯）
//   SHA-256       —— FIPS 180-4
//   HKDF-SHA256   —— RFC 5869 密钥派生
//   AES-256-GCM   —— FIPS 197 + NIST SP 800-38D（AEAD）
//
// 供 tight 传输层使用：握手阶段交换 X25519 公钥并 HKDF 派生会话密钥，
// 数据分组（Data/Parity/Command）以 AES-256-GCM 加密（报文头前 44 字节
// 作为 AAD 绑定防篡改）。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tight::tight_detail {

// ---------- X25519（ECDH） ----------

struct X25519KeyPair {
    std::array<std::uint8_t, 32> private_key;
    std::array<std::uint8_t, 32> public_key;
};

// 生成随机密钥对（内部使用 std::random_device）
X25519KeyPair x25519_generate();

// 计算 ECDH 共享秘密；对端公钥为低阶点（结果全零）时返回 false
bool x25519(std::array<std::uint8_t, 32>& out,
            const std::array<std::uint8_t, 32>& private_key,
            const std::array<std::uint8_t, 32>& peer_public_key);

// ---------- SHA-256 ----------

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len);

// ---------- HKDF-SHA256（extract + 单块 expand，输出 32 字节） ----------

std::array<std::uint8_t, 32> hkdf_sha256(const std::uint8_t* ikm, std::size_t ikm_len,
                                         const std::uint8_t* salt, std::size_t salt_len,
                                         const std::string& info);

// ---------- AES-256-GCM ----------

inline constexpr std::size_t kGcmNonceSize = 12;   // 96 位随机数（标准 GCM）
inline constexpr std::size_t kGcmTagSize = 16;     // 128 位认证标签

// 加密：ciphertext 与 plaintext 等长（调用方预留），tag 输出 16 字节
bool aes256_gcm_encrypt(const std::array<std::uint8_t, 32>& key,
                        const std::array<std::uint8_t, kGcmNonceSize>& nonce,
                        const std::uint8_t* aad, std::size_t aad_len,
                        const std::uint8_t* plaintext, std::size_t pt_len,
                        std::uint8_t* ciphertext,
                        std::uint8_t* tag);

// 解密并校验认证标签；标签不匹配返回 false（plaintext 内容无效）
bool aes256_gcm_decrypt(const std::array<std::uint8_t, 32>& key,
                        const std::array<std::uint8_t, kGcmNonceSize>& nonce,
                        const std::uint8_t* aad, std::size_t aad_len,
                        const std::uint8_t* ciphertext, std::size_t ct_len,
                        const std::uint8_t* tag,
                        std::uint8_t* plaintext);

} // namespace tight::tight_detail
