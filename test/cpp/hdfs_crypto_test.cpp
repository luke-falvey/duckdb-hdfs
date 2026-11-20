// Unit tests for the encryption-zone helpers (src/hdfs_crypto.cpp). No cluster
// or KMS needed. Build and run with `make test_unit`.

#include "hdfs_crypto.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/json_document.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace duckdb;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

std::string Hex(const std::string &hex) {
	std::string out;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		out += static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16));
	}
	return out;
}

// The message of the IOException `fn` throws ("" if it throws none).
template <class FN>
std::string IOError(FN &&fn) {
	try {
		fn();
	} catch (const IOException &e) {
		return ErrorData(e).RawMessage();
	}
	return std::string();
}

bool Contains(const std::string &haystack, const std::string &needle) {
	return haystack.find(needle) != std::string::npos;
}

std::string Transform(const std::string &key, const std::string &iv, uint64_t offset, std::string data) {
	HdfsCrypto::AesCtrTransform(key, iv, offset, reinterpret_cast<uint8_t *>(&data[0]), data.size());
	return data;
}

// NIST SP 800-38A F.5.1 (AES-128-CTR): the IV is the initial counter block.
void NistVector() {
	auto key = Hex("2b7e151628aed2a6abf7158809cf4f3c");
	auto iv = Hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
	auto plain = Hex("6bc1bee22e409f96e93d7e117393172a"
	                 "ae2d8a571e03ac9c9eb76fac45af8e51"
	                 "30c81c46a35ce411e5fbc1191a0a52ef"
	                 "f69f2445df4f9b17ad2b417be66c3710");
	auto cipher = Hex("874d6191b620e3261bef6864990db6ce"
	                  "9806f66b7970fdff8617187bb9fffdff"
	                  "5ae4df3edbd5d35e5b4f09020db03eab"
	                  "1e031dda2fbe03d1792170a0f3009cee");
	CHECK(Transform(key, iv, 0, plain) == cipher);
	CHECK(Transform(key, iv, 0, cipher) == plain); // CTR is its own inverse

	// Any slice at any offset, block-aligned or not, matches the whole-file
	// transform: this is what positional reads and appends rely on.
	for (uint64_t start = 0; start < plain.size(); start++) {
		for (uint64_t len = 1; start + len <= plain.size(); len += 7) {
			CHECK(Transform(key, iv, start, plain.substr(start, len)) == cipher.substr(start, len));
		}
	}
}

void CounterCarry() {
	// offset / 16 is added to the IV as a 128-bit big-endian number.
	auto iv = Hex("000000000000000000000000000000ff");
	CHECK(HdfsCrypto::CounterBlock(iv, 0) == iv);
	CHECK(HdfsCrypto::CounterBlock(iv, 15) == iv);
	CHECK(HdfsCrypto::CounterBlock(iv, 16) == Hex("00000000000000000000000000000100"));
	auto high = Hex("0000000000000000ffffffffffffffff");
	CHECK(HdfsCrypto::CounterBlock(high, 16) == Hex("00000000000000010000000000000000"));
	auto top = Hex("ffffffffffffffffffffffffffffffff");
	CHECK(HdfsCrypto::CounterBlock(top, 16) == Hex("00000000000000000000000000000000"));
}

void Base64() {
	CHECK(HdfsCrypto::Base64Encode("") == "");
	CHECK(HdfsCrypto::Base64Encode("f") == "Zg==");
	CHECK(HdfsCrypto::Base64Encode("fo") == "Zm8=");
	CHECK(HdfsCrypto::Base64Encode("foo") == "Zm9v");
	CHECK(HdfsCrypto::Base64Decode("Zm9vYmE=") == "fooba");
	CHECK(HdfsCrypto::Base64Decode("Zm9vYmE") == "fooba"); // unpadded
	std::string bytes = Hex("fbff");
	CHECK(HdfsCrypto::Base64Encode(bytes) == "+/8=");
	CHECK(HdfsCrypto::Base64Decode("-_8") == bytes); // URL-safe, as the KMS sends it
	bool threw = false;
	try {
		HdfsCrypto::Base64Decode("not base64!");
	} catch (const IOException &) {
		threw = true;
	}
	CHECK(threw);
}

void KmsUri() {
	CHECK(HdfsCrypto::ParseKmsUri("kms://http@kms1:9600/kms") == std::vector<std::string>({"http://kms1:9600/kms"}));
	CHECK(HdfsCrypto::ParseKmsUri("kms://https@kms1;kms2:9600/kms") ==
	      std::vector<std::string>({"https://kms1:9600/kms", "https://kms2:9600/kms"}));
	// The scheme is case-insensitive, as a URL scheme is.
	CHECK(HdfsCrypto::ParseKmsUri("kms://HTTPS@kms1:9600/kms") == std::vector<std::string>({"https://kms1:9600/kms"}));
	CHECK(Contains(IOError([] { HdfsCrypto::ParseKmsUri("jceks://file@/tmp/keystore"); }), "only kms://"));
	// Only http and https: the URL comes from the cluster config and must not reach local files or other protocols.
	for (auto uri : {"kms://file@/etc/passwd", "kms://gopher@kms1:70/kms", "kms://ftp@kms1/kms", "kms://@kms1/kms",
	                 "kms://http+unix@kms1/kms"}) {
		CHECK(Contains(IOError([&] { HdfsCrypto::ParseKmsUri(uri); }), "the scheme must be http or https"));
	}
}

// Even a URL that bypasses ParseKmsUri is refused by the HTTP client.
void KmsProtocols() {
	HdfsKeyCache cache;
	auto error = IOError([&] {
		HdfsCrypto::DecryptEncryptedKey(cache, {"file:///etc/passwd#"}, false, "alice", "k1", "k1@0",
		                                std::string(16, 'i'), std::string(16, 'k'));
	});
	CHECK(Contains(error, "KMS request to file:///etc/passwd"));
	CHECK(cache.Size() == 0);
	CHECK(Contains(IOError([&] {
		               HdfsCrypto::DecryptEncryptedKey(cache, {}, false, "alice", "k1", "k1@0", std::string(16, 'i'),
		                                               std::string(16, 'k'));
	               }),
	               "no KMS URL configured"));
}

// The error message a KMS refusal reports, in Hadoop's JSON shape.
void KmsErrors() {
	CHECK(HdfsCrypto::KmsErrorMessage(
	          R"({"RemoteException":{"message":"User:bob not allowed to do 'DECRYPT_EEK' on 'k1'",)"
	          R"("exception":"AuthorizationException"}})") == "User:bob not allowed to do 'DECRYPT_EEK' on 'k1'");
	CHECK(HdfsCrypto::KmsErrorMessage(R"({"message":"Key k1 not found"})") == "Key k1 not found");
	CHECK(HdfsCrypto::KmsErrorMessage("<html>Bad Gateway</html>").empty());
	CHECK(HdfsCrypto::KmsErrorMessage("").empty());
	CHECK(HdfsCrypto::KmsErrorMessage("[\"message\"]").empty());
	// Escapes, including \uXXXX, are decoded.
	CHECK(HdfsCrypto::KmsErrorMessage(R"({"message":"caf\u00e9 \"k1\"\\x"})") == "caf\xc3\xa9 \"k1\"\\x");
	// Only the member itself counts, not the same text inside another string, or a nested object's member.
	CHECK(HdfsCrypto::KmsErrorMessage(R"({"note":"\"message\": \"fake\"","message":"real"})") == "real");
	CHECK(HdfsCrypto::KmsErrorMessage(R"({"other":{"message":"nested"}})").empty());
	CHECK(HdfsCrypto::KmsErrorMessage(R"({"message":42})").empty());
}

// The decrypt request body, and the key in the response.
void KmsMessages() {
	std::string name = "we\"ird\\name\n\x01";
	auto request = HdfsCrypto::KmsDecryptRequest(name, Hex("00ff"), "key");
	auto doc = JSONDocument::Parse(request.data(), request.size());
	CHECK(doc->GetRoot().GetMember("name").GetString() == name);
	CHECK(doc->GetRoot().GetMember("iv").GetString() == "AP8=");
	CHECK(doc->GetRoot().GetMember("material").GetString() == "a2V5");

	auto key = Hex("000102030405060708090a0b0c0d0e0f");
	CHECK(HdfsCrypto::KmsDecryptedKey(R"({"name":"EK","material":")" + HdfsCrypto::Base64Encode(key) + R"("})") == key);
	// A "material" inside another string or object isn't the key.
	CHECK(Contains(IOError([] { HdfsCrypto::KmsDecryptedKey(R"({"name":"\"material\":\"AAAA\""})"); }),
	               "no decrypted key"));
	CHECK(Contains(IOError([] { HdfsCrypto::KmsDecryptedKey(R"({"x":{"material":"AAAA"}})"); }), "no decrypted key"));
	CHECK(Contains(IOError([] { HdfsCrypto::KmsDecryptedKey("not json"); }), "no decrypted key"));
	CHECK(Contains(IOError([] { HdfsCrypto::KmsDecryptedKey(R"({"material":null})"); }), "no decrypted key"));
}

void KeyCache() {
	{
		HdfsKeyCache cache;
		CHECK(cache.Get("a").empty());
		cache.Put("a", "key-a");
		CHECK(cache.Get("a") == "key-a");
		cache.Put("a", "key-a2"); // replaced
		CHECK(cache.Get("a") == "key-a2");
		CHECK(cache.Size() == 1);
		// A full cache with nothing expired starts over.
		for (idx_t i = 0; i < HdfsKeyCache::LIMIT; i++) {
			cache.Put("k" + std::to_string(i), "v");
		}
		CHECK(cache.Size() == 1);
		CHECK(cache.Get("k" + std::to_string(HdfsKeyCache::LIMIT - 1)) == "v");
	}
	{
		// Entries expire after the TTL, and are dropped when looked up.
		HdfsKeyCache cache(std::chrono::milliseconds(1));
		cache.Put("a", "key-a");
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		CHECK(cache.Get("a").empty());
		CHECK(cache.Size() == 0);
	}
	{
		// A full cache drops its expired entries first.
		HdfsKeyCache cache(std::chrono::milliseconds(50));
		for (idx_t i = 0; i + 1 < HdfsKeyCache::LIMIT; i++) {
			cache.Put("old" + std::to_string(i), "v");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(60));
		cache.Put("fresh", "v");
		cache.Put("fresh2", "v"); // full now: the expired entries go, the fresh one stays
		CHECK(cache.Size() == 2);
		CHECK(cache.Get("fresh") == "v");
	}
	std::string secret = "0123456789abcdef0123456789abcdef";
	HdfsCrypto::Wipe(secret);
	CHECK(secret == std::string(32, '\0'));
}

} // namespace

int main() {
	NistVector();
	CounterCarry();
	Base64();
	KmsUri();
	KmsProtocols();
	KmsErrors();
	KmsMessages();
	KeyCache();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("All crypto tests passed\n");
	return 0;
}
