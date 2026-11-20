#include "hdfs_crypto.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/json_document.hpp"

#include <curl/curl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <memory>
#include <utility>

namespace duckdb {

namespace {

// --- base64, JSON and URLs--------------------------------------------------------

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

// The string member `key` of a JSON object, or "" if `object` isn't one or has none.
string StringMember(const JSONValue &object, const string &key) {
	auto member = object.GetMember(key);
	return member.IsString() ? member.GetString() : string();
}

string UrlEncode(const string &s) {
	static const char *hex = "0123456789ABCDEF";
	string out;
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

// --- HTTP ------------------------------------------------------------------------------

// KMS responses are small JSON documents; anything larger is refused.
constexpr size_t MAX_RESPONSE_SIZE = 1 << 20;

size_t AppendBody(char *ptr, size_t size, size_t nmemb, void *userdata) {
	auto &body = *static_cast<string *>(userdata);
	if (body.size() + size * nmemb > MAX_RESPONSE_SIZE) {
		return 0; // makes libcurl fail the transfer
	}
	body.append(ptr, size * nmemb);
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
	string body;
};

HttpResponse HttpPostJson(const string &url, const string &payload, bool spnego) {
	static std::once_flag init;
	std::call_once(init, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
	std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
	if (!curl) {
		throw IOException("could not initialize libcurl");
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
	// Only http(s), also for redirects (which aren't followed anyway): the URL comes from the cluster config.
#if LIBCURL_VERSION_NUM >= 0x075500
	curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
	curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
	curl_easy_setopt(h, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
	curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
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
		throw IOException("KMS request to " + url + " failed: " + curl_easy_strerror(rc));
	}
	curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &response.status);
	return response;
}

} // namespace

constexpr std::chrono::minutes HdfsKeyCache::DEFAULT_TTL;
constexpr idx_t HdfsKeyCache::LIMIT;

HdfsKeyCache::HdfsKeyCache(std::chrono::steady_clock::duration ttl_p) : ttl(ttl_p) {
}

HdfsKeyCache::~HdfsKeyCache() {
	for (auto &entry : entries) {
		HdfsCrypto::Wipe(entry.second.key);
	}
}

string HdfsKeyCache::Get(const string &id) {
	std::lock_guard<std::mutex> lock(mutex);
	auto it = entries.find(id);
	if (it == entries.end()) {
		return string();
	}
	if (it->second.expiry > std::chrono::steady_clock::now()) {
		return it->second.key;
	}
	HdfsCrypto::Wipe(it->second.key);
	entries.erase(it);
	return string();
}

void HdfsKeyCache::Put(const string &id, const string &key) {
	auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(mutex);
	if (entries.size() >= LIMIT) {
		// Expired entries first; if none are, start over.
		bool all = std::none_of(entries.begin(), entries.end(), [&](const std::pair<const string, Entry> &entry) {
			return entry.second.expiry <= now;
		});
		for (auto it = entries.begin(); it != entries.end();) {
			if (all || it->second.expiry <= now) {
				HdfsCrypto::Wipe(it->second.key);
				it = entries.erase(it);
			} else {
				++it;
			}
		}
	}
	auto &entry = entries[id];
	HdfsCrypto::Wipe(entry.key);
	entry.key = key;
	entry.expiry = now + ttl;
}

idx_t HdfsKeyCache::Size() {
	std::lock_guard<std::mutex> lock(mutex);
	return entries.size();
}

void HdfsCrypto::Wipe(string &secret) {
	if (!secret.empty()) {
		OPENSSL_cleanse(&secret[0], secret.size());
	}
}

string HdfsCrypto::Base64Encode(const string &data) {
	string out;
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

string HdfsCrypto::Base64Decode(const string &text) {
	string out;
	uint32_t acc = 0;
	int bits = 0;
	for (char c : text) {
		if (c == '=' || c == '\n' || c == '\r' || c == ' ') {
			continue;
		}
		int v = B64Value(c);
		if (v < 0) {
			throw IOException("invalid base64 in KMS response");
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

vector<string> HdfsCrypto::ParseKmsUri(const string &uri) {
	// kms://<scheme>@<host>[;<host>...][:<port>]<path>
	const string prefix = "kms://";
	if (uri.rfind(prefix, 0) != 0) {
		throw IOException("unsupported key provider '" + uri + "' (only kms:// providers are supported)");
	}
	auto at = uri.find('@', prefix.size());
	if (at == string::npos) {
		throw IOException("invalid KMS URI '" + uri + "'");
	}
	string scheme = uri.substr(prefix.size(), at - prefix.size());
	std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
	if (scheme != "http" && scheme != "https") {
		throw IOException("unsupported KMS URI '" + uri + "': the scheme must be http or https");
	}
	auto path_start = uri.find('/', at + 1);
	string authority = uri.substr(at + 1, path_start == string::npos ? string::npos : path_start - at - 1);
	string path = path_start == string::npos ? string() : uri.substr(path_start);
	string port;
	auto colon = authority.rfind(':');
	if (colon != string::npos) {
		port = authority.substr(colon);
		authority.erase(colon);
	}
	vector<string> urls;
	size_t start = 0;
	while (start <= authority.size()) {
		auto semi = authority.find(';', start);
		auto host = authority.substr(start, semi == string::npos ? string::npos : semi - start);
		if (!host.empty()) {
			urls.push_back(scheme + "://" + host + port + path);
		}
		if (semi == string::npos) {
			break;
		}
		start = semi + 1;
	}
	if (urls.empty()) {
		throw IOException("invalid KMS URI '" + uri + "'");
	}
	return urls;
}

string HdfsCrypto::KmsErrorMessage(const string &body) {
	JSONParseError error;
	auto doc = JSONDocument::TryParse(body.data(), body.size(), error);
	if (!doc) {
		return string();
	}
	auto root = doc->GetRoot();
	auto message = StringMember(root.GetMember("RemoteException"), "message");
	return message.empty() ? StringMember(root, "message") : message;
}

string HdfsCrypto::KmsDecryptRequest(const string &key_name, const string &iv, const string &encrypted_key) {
	JSONWriter writer;
	auto request = writer.CreateObject();
	request.AddString("name", key_name);
	request.AddString("iv", Base64Encode(iv));
	request.AddString("material", Base64Encode(encrypted_key));
	writer.SetRoot(request);
	return writer.ToString();
}

string HdfsCrypto::KmsDecryptedKey(const string &body) {
	JSONParseError error;
	auto doc = JSONDocument::TryParse(body.data(), body.size(), error);
	if (!doc || !doc->GetRoot().GetMember("material").IsString()) {
		throw IOException("KMS response has no decrypted key (\"material\")");
	}
	auto material = doc->GetRoot().GetMember("material").GetString();
	string key;
	try {
		key = Base64Decode(material);
	} catch (...) {
		Wipe(material);
		throw;
	}
	Wipe(material);
	return key;
}

string HdfsCrypto::CurrentOsUser() {
	struct passwd pw;
	struct passwd *result = nullptr;
	vector<char> buffer(16384);
	if (getpwuid_r(getuid(), &pw, buffer.data(), buffer.size(), &result) != 0 || !result || !result->pw_name) {
		return string();
	}
	return string(result->pw_name);
}

string HdfsCrypto::DecryptEncryptedKey(HdfsKeyCache &cache, const vector<string> &kms_urls, bool kerberos,
                                       const string &user, const string &key_name, const string &key_version_name,
                                       const string &iv, const string &encrypted_key) {
	string cache_id = key_version_name + '\0' + iv + '\0' + encrypted_key;
	auto cached = cache.Get(cache_id);
	if (!cached.empty()) {
		return cached;
	}
	string effective_user = kerberos ? string() : user;
	string payload = KmsDecryptRequest(key_name, iv, encrypted_key);
	string last_error = "no KMS URL configured";
	for (auto &base : kms_urls) {
		string url = base + "/v1/keyversion/" + UrlEncode(key_version_name) + "/_eek?eek_op=decrypt";
		if (!effective_user.empty()) {
			url += "&user.name=" + UrlEncode(effective_user);
		}
		HttpResponse response;
		try {
			response = HttpPostJson(url, payload, kerberos);
		} catch (const IOException &e) {
			last_error = ErrorData(e).RawMessage();
			continue; // try the next KMS instance
		}
		if (response.status != 200) {
			auto message = KmsErrorMessage(response.body);
			auto error = "KMS refused to decrypt the key for '" + key_name + "' (HTTP " +
			             std::to_string(response.status) + (message.empty() ? "" : ": " + message) + ")";
			if (response.status >= 500) {
				// A failing KMS instance: try the next one.
				last_error = error;
				continue;
			}
			throw IOException(error);
		}
		string key;
		try {
			key = KmsDecryptedKey(response.body);
		} catch (...) {
			Wipe(response.body);
			throw;
		}
		Wipe(response.body);
		if (key.size() != 16 && key.size() != 32) {
			Wipe(key);
			throw IOException("KMS returned an invalid data encryption key for '" + key_name + "'");
		}
		cache.Put(cache_id, key);
		return key;
	}
	throw IOException(last_error);
}

string HdfsCrypto::CounterBlock(const string &iv, uint64_t offset) {
	if (iv.size() != 16) {
		throw IOException("invalid encryption IV length");
	}
	string block = iv;
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

void HdfsCrypto::AesCtrTransform(const string &key, const string &iv, uint64_t offset, uint8_t *data, size_t len) {
	const EVP_CIPHER *cipher = key.size() == 32 ? EVP_aes_256_ctr() : key.size() == 16 ? EVP_aes_128_ctr() : nullptr;
	if (!cipher) {
		throw IOException("unsupported data encryption key length");
	}
	std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
	auto counter = CounterBlock(iv, offset);
	if (!ctx || EVP_EncryptInit_ex(ctx.get(), cipher, nullptr, reinterpret_cast<const unsigned char *>(key.data()),
	                               reinterpret_cast<const unsigned char *>(counter.data())) != 1) {
		throw IOException("could not initialize AES-CTR");
	}
	int out_len = 0;
	// Skip the keystream bytes before `offset` within its 16-byte block.
	unsigned char skip[16] = {0};
	auto padding = static_cast<int>(offset % 16);
	if (padding > 0 && EVP_EncryptUpdate(ctx.get(), skip, &out_len, skip, padding) != 1) {
		throw IOException("AES-CTR failed");
	}
	while (len > 0) {
		auto chunk = static_cast<int>(len > static_cast<size_t>(INT_MAX / 2) ? INT_MAX / 2 : len);
		if (EVP_EncryptUpdate(ctx.get(), data, &out_len, data, chunk) != 1) {
			throw IOException("AES-CTR failed");
		}
		data += chunk;
		len -= static_cast<size_t>(chunk);
	}
}

} // namespace duckdb
