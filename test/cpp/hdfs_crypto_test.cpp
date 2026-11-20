// Unit tests for the encryption-zone helpers (src/hdfs_crypto.cpp). No cluster
// or KMS needed. Build and run with `make test_unit`.

#include "hdfs_crypto.hpp"

#include "duckdb/common/exception.hpp"

#include <cstdio>
#include <string>
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
	bool threw = false;
	try {
		HdfsCrypto::ParseKmsUri("jceks://file@/tmp/keystore");
	} catch (const IOException &) {
		threw = true;
	}
	CHECK(threw);
}

// The error message a KMS refusal reports, in Hadoop's JSON shape.
void KmsErrors() {
	CHECK(HdfsCrypto::KmsErrorMessage(
	          R"({"RemoteException":{"message":"User:bob not allowed to do 'DECRYPT_EEK' on 'k1'",)"
	          R"("exception":"AuthorizationException"}})") == "User:bob not allowed to do 'DECRYPT_EEK' on 'k1'");
	CHECK(HdfsCrypto::KmsErrorMessage(R"({"message":"Key k1 not found"})") == "Key k1 not found");
	CHECK(HdfsCrypto::KmsErrorMessage("<html>Bad Gateway</html>").empty());
}

} // namespace

int main() {
	NistVector();
	CounterCarry();
	Base64();
	KmsUri();
	KmsErrors();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("All crypto tests passed\n");
	return 0;
}
