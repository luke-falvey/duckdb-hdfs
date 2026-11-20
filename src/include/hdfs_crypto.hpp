//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_crypto.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Client-side encryption for HDFS encryption zones (Transparent Data Encryption), as in the Hadoop client: each
//! file has a data encryption key that the NameNode hands out encrypted; the KMS decrypts it, and the file contents
//! are encrypted with AES-CTR. Failures throw IOException.
struct HdfsCrypto {
	//! Hadoop's CipherSuiteProto / CryptoProtocolVersionProto values that are supported.
	static constexpr int SUITE_AES_CTR_NOPADDING = 2;
	static constexpr int PROTOCOL_ENCRYPTION_ZONES = 2;

	//! Expand a KMS provider URI ("kms://http@host1;host2:9600/kms") into base URLs ("http://host1:9600/kms", ...).
	static vector<string> ParseKmsUri(const string &uri);

	//! Ask the KMS to decrypt an encrypted data encryption key; `kms_urls` are tried in order (a 5xx answer moves on
	//! to the next). With `kerberos` the request authenticates with SPNEGO, otherwise `user` is sent (simple auth).
	//! Decrypted keys are cached for a few minutes per `cache_scope` (one client connection: its identity and key
	//! provider), so reopening a file doesn't call the KMS again.
	static string DecryptEncryptedKey(const string &cache_scope, const vector<string> &kms_urls, bool kerberos,
	                                  const string &user, const string &key_name, const string &key_version_name,
	                                  const string &iv, const string &encrypted_key);

	//! AES-CTR encrypt or decrypt (the same operation) `len` bytes in place, where `data` sits at byte `offset` of
	//! the file. `key` is 16 or 32 bytes; `iv` is the file's 16-byte IV.
	static void AesCtrTransform(const string &key, const string &iv, uint64_t offset, uint8_t *data, size_t len);

	//! The 16-byte counter block for `offset`: the file IV plus offset / 16, as a 128-bit big-endian sum (Hadoop's
	//! AesCtrCryptoCodec.calculateIV).
	static string CounterBlock(const string &iv, uint64_t offset);

	//! Base64; decoding accepts the standard and the URL-safe alphabet, with or without padding.
	static string Base64Encode(const string &data);
	static string Base64Decode(const string &text);

	//! The message of a KMS error response: Hadoop's {"RemoteException": {"message": ...}}, or a flat
	//! {"message": ...}; "" if there is none.
	static string KmsErrorMessage(const string &body);

	//! The current OS account's name ("" if unknown).
	static string CurrentOsUser();
};

} // namespace duckdb
