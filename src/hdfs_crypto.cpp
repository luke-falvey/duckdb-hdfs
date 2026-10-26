#include "hdfs_crypto.hpp"

#include <curl/curl.h>
#include <openssl/evp.h>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace hdfs_crypto {

namespace {

// --- base64 and JSON -----------------------------------------------------------

const char *const B64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int B64Value(char c) {
	if (c >= 'A' && c <= 'Z') {
		return c - 'A';
	}
	if (c >= 'a' && c <= 'z') {
		return c - 'a' + 26;
	}
	if (c >= '0' && c <= '9') {
		return c - '0' + 52;
	}
	if (c == '+' || c == '-') {
		return 62;
	}
	if (c == '/' || c == '_') {
		return 63;
	}
	return -1;
}

std::string JsonEscape(const std::string &s) {
	std::string out;
	for (char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		default:
			out += c;
		}
	}
	return out;
}

// The string value of `"field": "..."` in a flat JSON object, or "" if absent.
std::string JsonStringField(const std::string &json, const std::string &field) {
	auto key = json.find("\"" + field + "\"");
	if (key == std::string::npos) {
		return std::string();
	}
	auto colon = json.find(':', key + field.size() + 2);
	auto open = colon == std::string::npos ? colon : json.find('"', colon + 1);
	if (open == std::string::npos) {
		return std::string();
	}
	std::string out;
	for (size_t i = open + 1; i < json.size(); i++) {
		if (json[i] == '\\' && i + 1 < json.size()) {
			out += json[++i];
		} else if (json[i] == '"') {
			return out;
		} else {
			out += json[i];
		}
	}
	return std::string();
}

std::string UrlEncode(const std::string &s) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += static_cast<char>(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string OsUser() {
	struct passwd *pw = getpwuid(getuid());
	return pw && pw->pw_name ? std::string(pw->pw_name) : std::string();
}

// --- HTTP ------------------------------------------------------------------------------

size_t AppendBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
	static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
	return size * nmemb;
}

bool IsRegularFile(const char *path) {
	struct stat st;
	return ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// A CA bundle for https KMS endpoints: the usual environment overrides, then
// the common system locations (libcurl's built-in default may not exist here).
const char *CaBundle() {
	for (const char *env : {"CURL_CA_BUNDLE", "SSL_CERT_FILE"}) {
		const char *value = std::getenv(env);
		if (value && *value) {
			return value;
		}
	}
	static const char *candidates[] = {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
	                                   "/etc/ssl/cert.pem", "/etc/ssl/ca-bundle.pem"};
	for (const char *path : candidates) {
		if (IsRegularFile(path)) {
			return path;
		}
	}
	return nullptr;
}

struct HttpResponse {
	long status = 0; // NOLINT(google-runtime-int): libcurl reports the status as a long
	std::string body;
};

HttpResponse HttpPostJson(const std::string &url, const std::string &payload, bool spnego) {
	static std::once_flag init;
	std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl) {
		throw CryptoError("could not initialize libcurl");
	}
	HttpResponse response;
	struct curl_slist *headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, "Accept: application/json");
	CURL *h = curl.get();
	curl_easy_setopt(h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
	curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, AppendBody);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &response.body);
	curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
	curl_easy_setopt(h, CURLOPT_TIMEOUT, 60L);
	curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
	if (spnego) {
		// SPNEGO with the Kerberos ticket cache: no user name or password.
		curl_easy_setopt(h, CURLOPT_HTTPAUTH, CURLAUTH_NEGOTIATE);
		curl_easy_setopt(h, CURLOPT_USERPWD, ":");
	}
	if (const char *ca = CaBundle()) {
		curl_easy_setopt(h, CURLOPT_CAINFO, ca);
	}
	CURLcode rc = curl_easy_perform(h);
	curl_slist_free_all(headers);
	if (rc != CURLE_OK) {
		throw CryptoError("KMS request to " + url + " failed: " + curl_easy_strerror(rc));
	}
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &response.status);
	return response;
}

// --- decrypted key cache -----------------------------------------------------------------

struct KeyCache {
	std::mutex mutex;
	std::map<std::string, std::string> keys;
	static constexpr size_t LIMIT = 4096;
};

KeyCache &DecryptedKeys() {
	static KeyCache cache;
	return cache;
}

} // namespace

std::string Base64Encode(const std::string &data) {
	std::string out;
	size_t i = 0;
	auto bytes = reinterpret_cast<const unsigned char *>(data.data());
	for (; i + 2 < data.size(); i += 3) {
		uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
		out += B64_CHARS[(v >> 18) & 63];
		out += B64_CHARS[(v >> 12) & 63];
		out += B64_CHARS[(v >> 6) & 63];
		out += B64_CHARS[v & 63];
	}
	if (i + 1 == data.size()) {
		uint32_t v = bytes[i] << 16;
		out += B64_CHARS[(v >> 18) & 63];
		out += B64_CHARS[(v >> 12) & 63];
		out += "==";
	} else if (i + 2 == data.size()) {
		uint32_t v = (bytes[i] << 16) | (bytes[i + 1] << 8);
		out += B64_CHARS[(v >> 18) & 63];
		out += B64_CHARS[(v >> 12) & 63];
		out += B64_CHARS[(v >> 6) & 63];
		out += '=';
	}
	return out;
}

std::string Base64Decode(const std::string &text) {
	std::string out;
	uint32_t acc = 0;
	int bits = 0;
	for (char c : text) {
		if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
			continue;
		}
		int v = B64Value(c);
		if (v < 0) {
			throw CryptoError("invalid base64 in KMS response");
		}
		acc = (acc << 6) | static_cast<uint32_t>(v);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out += static_cast<char>((acc >> bits) & 0xFF);
		}
	}
	return out;
}

std::vector<std::string> ParseKmsUri(const std::string &uri) {
	// kms://<scheme>@<host>[;<host>...][:<port>]<path>
	const std::string prefix = "kms://";
	if (uri.rfind(prefix, 0) != 0) {
		throw CryptoError("unsupported key provider '" + uri + "' (only kms:// providers are supported)");
	}
	auto at = uri.find('@', prefix.size());
	if (at == std::string::npos) {
		throw CryptoError("invalid KMS URI '" + uri + "'");
	}
	std::string scheme = uri.substr(prefix.size(), at - prefix.size());
	auto path_start = uri.find('/', at + 1);
	std::string authority =
	    uri.substr(at + 1, path_start == std::string::npos ? std::string::npos : path_start - at - 1);
	std::string path = path_start == std::string::npos ? std::string() : uri.substr(path_start);
	std::string port;
	auto colon = authority.rfind(':');
	if (colon != std::string::npos) {
		port = authority.substr(colon);
		authority.erase(colon);
	}
	std::vector<std::string> urls;
	size_t start = 0;
	while (start <= authority.size()) {
		auto semi = authority.find(';', start);
		auto host = authority.substr(start, semi == std::string::npos ? std::string::npos : semi - start);
		if (!host.empty()) {
			urls.push_back(scheme + "://" + host + port + path);
		}
		if (semi == std::string::npos) {
			break;
		}
		start = semi + 1;
	}
	if (urls.empty()) {
		throw CryptoError("invalid KMS URI '" + uri + "'");
	}
	return urls;
}

std::string DecryptEncryptedKey(const std::vector<std::string> &kms_urls, bool kerberos, const std::string &user,
                                const std::string &key_name, const std::string &key_version_name, const std::string &iv,
                                const std::string &encrypted_key) {
	std::string cache_key = key_version_name + '\0' + iv + '\0' + encrypted_key;
	{
		auto &cache = DecryptedKeys();
		std::lock_guard<std::mutex> lock(cache.mutex);
		auto it = cache.keys.find(cache_key);
		if (it != cache.keys.end()) {
			return it->second;
		}
	}
	std::string effective_user = kerberos ? std::string() : user.empty() ? OsUser() : user;
	std::string payload = "{\"name\":\"" + JsonEscape(key_name) + "\",\"iv\":\"" + Base64Encode(iv) +
	                      "\",\"material\":\"" + Base64Encode(encrypted_key) + "\"}";
	std::string last_error = "no KMS URL configured";
	for (auto &base : kms_urls) {
		std::string url = base + "/v1/keyversion/" + UrlEncode(key_version_name) + "/_eek?eek_op=decrypt";
		if (!effective_user.empty()) {
			url += "&user.name=" + UrlEncode(effective_user);
		}
		HttpResponse response;
		try {
			response = HttpPostJson(url, payload, kerberos);
		} catch (const CryptoError &e) {
			last_error = e.what();
			continue; // try the next KMS instance
		}
		if (response.status != 200) {
			auto message = JsonStringField(response.body, "RemoteException");
			if (message.empty()) {
				message = JsonStringField(response.body, "message");
			}
			throw CryptoError("KMS refused to decrypt the key for '" + key_name + "' (HTTP " +
			                  std::to_string(response.status) + (message.empty() ? "" : ": " + message) + ")");
		}
		auto material = Base64Decode(JsonStringField(response.body, "material"));
		if (material.size() != 16 && material.size() != 32) {
			throw CryptoError("KMS returned an invalid data encryption key for '" + key_name + "'");
		}
		auto &cache = DecryptedKeys();
		std::lock_guard<std::mutex> lock(cache.mutex);
		if (cache.keys.size() >= KeyCache::LIMIT) {
			cache.keys.clear();
		}
		cache.keys.emplace(std::move(cache_key), material);
		return material;
	}
	throw CryptoError(last_error);
}

std::string CounterBlock(const std::string &iv, uint64_t offset) {
	if (iv.size() != 16) {
		throw CryptoError("invalid encryption IV length");
	}
	std::string block = iv;
	uint64_t counter = offset / 16;
	unsigned sum = 0;
	for (int i = 15; i >= 0; i--) {
		sum = static_cast<unsigned char>(iv[i]) + (sum >> 8);
		if (i >= 8) {
			sum += counter & 0xFF;
			counter >>= 8;
		}
		block[i] = static_cast<char>(sum & 0xFF);
	}
	return block;
}

void AesCtrTransform(const std::string &key, const std::string &iv, uint64_t offset, uint8_t *data, size_t len) {
	const EVP_CIPHER *cipher = key.size() == 32 ? EVP_aes_256_ctr() : key.size() == 16 ? EVP_aes_128_ctr() : nullptr;
	if (!cipher) {
		throw CryptoError("unsupported data encryption key length");
	}
	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
	auto counter = CounterBlock(iv, offset);
	if (!ctx || EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
	                               reinterpret_cast<const unsigned char *>(counter.data())) != 1) {
		throw CryptoError("could not initialize AES-CTR");
	}
	int out_len = 0;
	// Skip the keystream bytes before `offset` within its 16-byte block.
	unsigned char skip[16] = {0};
	auto padding = static_cast<int>(offset % 16);
	if (padding > 0 && EVP_EncryptUpdate(ctx.get(), skip, &out_len, skip, padding) != 1) {
		throw CryptoError("AES-CTR failed");
	}
	while (len > 0) {
		auto chunk = static_cast<int>(len > static_cast<size_t>(INT_MAX / 2) ? INT_MAX / 2 : len);
		if (EVP_EncryptUpdate(ctx.get(), data, &out_len, data, chunk) != 1) {
			throw CryptoError("AES-CTR failed");
		}
		data += chunk;
		len -= static_cast<size_t>(chunk);
	}
}

} // namespace hdfs_crypto
