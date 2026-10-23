#pragma once

// Client-side encryption for HDFS encryption zones (Transparent Data
// Encryption), as in the Hadoop client: each file has a data encryption key
// (DEK) that the NameNode hands out encrypted (the EDEK); the client asks the
// KMS to decrypt it, then encrypts/decrypts the file contents with AES-CTR.

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace hdfs_crypto {

class CryptoError : public std::runtime_error {
public:
	explicit CryptoError(const std::string &message) : std::runtime_error(message) {
	}
};

// Hadoop's CipherSuiteProto / CryptoProtocolVersionProto values we support.
constexpr int SUITE_AES_CTR_NOPADDING = 2;
constexpr int PROTOCOL_ENCRYPTION_ZONES = 2;

// Expand a Hadoop KMS provider URI ("kms://http@host1;host2:9600/kms") into
// base URLs ("http://host1:9600/kms", ...). Throws CryptoError if it isn't a
// kms:// URI.
std::vector<std::string> ParseKmsUri(const std::string &uri);

// Ask the KMS to decrypt an encrypted data encryption key. `kms_urls` are tried
// in order. `user` is sent for simple authentication (empty: the OS user).
// Returns the raw key. Decrypted keys are cached, so reopening a file doesn't
// call the KMS again.
std::string DecryptEncryptedKey(const std::vector<std::string> &kms_urls, const std::string &user,
                                const std::string &key_name, const std::string &key_version_name, const std::string &iv,
                                const std::string &encrypted_key);

// AES-CTR encrypt or decrypt (the same operation) `len` bytes in place, where
// `data` sits at byte `offset` of the file. `key` is 16 or 32 bytes; `iv` is
// the file's 16-byte IV.
void AesCtrTransform(const std::string &key, const std::string &iv, uint64_t offset, uint8_t *data, size_t len);

// The 16-byte counter block for `offset`: the file IV plus offset / 16, as a
// 128-bit big-endian sum (Hadoop's AesCtrCryptoCodec.calculateIV).
std::string CounterBlock(const std::string &iv, uint64_t offset);

// Base64 helpers (exposed for tests). Decode accepts the standard and the
// URL-safe alphabet, with or without padding.
std::string Base64Encode(const std::string &data);
std::string Base64Decode(const std::string &text);

} // namespace hdfs_crypto
