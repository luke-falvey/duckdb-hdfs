//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_crypto.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

#include <chrono>
#include <map>
#include <mutex>

namespace duckdb {

//! Decrypted data encryption keys of one HDFS client (one connection: its identity and key provider), so reopening
//! a file doesn't call the KMS again. Owned by the client, so the keys go when the client does (at the latest when
//! the database closes). Keys are wiped from memory when they expire, are evicted or the cache is destroyed.
//! Thread-safe.
class HdfsKeyCache {
public:
	//! Bounds how long a revoked KMS permission keeps working for an open client.
	static constexpr std::chrono::minutes DEFAULT_TTL {10};
	//! Entries kept at most; inserting into a full cache first drops the expired ones, then all.
	static constexpr idx_t LIMIT = 4096;

	explicit HdfsKeyCache(std::chrono::steady_clock::duration ttl = DEFAULT_TTL);
	~HdfsKeyCache();
	HdfsKeyCache(const HdfsKeyCache &) = delete;
	HdfsKeyCache &operator=(const HdfsKeyCache &) = delete;

	//! The unexpired key cached under `id`, or "".
	string Get(const string &id);
	void Put(const string &id, const string &key);
	idx_t Size();

private:
	struct Entry {
		string key;
		std::chrono::steady_clock::time_point expiry;
	};
	const std::chrono::steady_clock::duration ttl;
	std::mutex mutex;
	std::map<string, Entry> entries;
};

//! Client-side encryption for HDFS encryption zones (Transparent Data Encryption), as in the Hadoop client: each
//! file has a data encryption key that the NameNode hands out encrypted; the KMS decrypts it, and the file contents
//! are encrypted with AES-CTR. Failures throw IOException.
struct HdfsCrypto {
	//! Hadoop's CipherSuiteProto / CryptoProtocolVersionProto values that are supported.
	static constexpr int SUITE_AES_CTR_NOPADDING = 2;
	static constexpr int PROTOCOL_ENCRYPTION_ZONES = 2;

	//! Expand a KMS provider URI ("kms://http@host1;host2:9600/kms") into base URLs ("http://host1:9600/kms", ...).
	//! Only the http and https schemes are accepted.
	static vector<string> ParseKmsUri(const string &uri);

	//! Ask the KMS to decrypt an encrypted data encryption key; `kms_urls` are tried in order (a 5xx answer moves on
	//! to the next). With `kerberos` the request authenticates with SPNEGO, otherwise `user` is sent (simple auth).
	//! Decrypted keys are cached in `cache`. The KMS is called with libcurl, not DuckDB's HTTP stack (which can't do
	//! SPNEGO), so DuckDB's http_proxy and CA settings don't apply; libcurl's proxy environment variables do.
	static string DecryptEncryptedKey(HdfsKeyCache &cache, const vector<string> &kms_urls, bool kerberos,
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
	//! {"message": ...}; "" if there is none (or the body isn't JSON).
	static string KmsErrorMessage(const string &body);
	//! The JSON body of a KMS decrypt request.
	static string KmsDecryptRequest(const string &key_name, const string &iv, const string &encrypted_key);
	//! The decrypted key in a KMS decrypt response ({"material": "<base64>", ...}). Throws if there is none.
	static string KmsDecryptedKey(const string &body);

	//! SHA-256 of `data`, as 64 lowercase hex digits.
	static string Sha256Hex(const string &data);

	//! Overwrite a secret's bytes with zeros (in a way the compiler can't drop).
	static void Wipe(string &secret);

	//! The current OS account's name ("" if unknown).
	static string CurrentOsUser();
};

} // namespace duckdb
