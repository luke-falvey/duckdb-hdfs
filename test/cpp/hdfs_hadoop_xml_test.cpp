// Unit tests for the Hadoop configuration parser (src/hdfs_hadoop_xml.cpp), which config_snapshot secrets use to
// capture a CONFIG_DIR. It must read a file exactly as libhdfs3 does (XmlConfig.cpp readConfigItems), since a
// CONFIG_DIR secret on the same directory is read by libhdfs3. Build and run with `make test_unit`.

// libhdfs3's own parser, to check that both read every case the same way.
#include "absl_log_undef.h"
#include "common/XmlConfig.h"

#include "hdfs_hadoop_xml.hpp"

#include "duckdb/common/exception.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

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

map<string, string> Parse(const string &xml) {
	map<string, string> properties;
	HdfsParseHadoopXml(xml, "test.xml", properties);
	return properties;
}

//! The message of the InvalidInputException parsing `xml` throws, or "" if it doesn't.
string ParseError(const string &xml) {
	try {
		Parse(xml);
	} catch (const InvalidInputException &e) {
		return e.what();
	}
	return string();
}

bool Contains(const string &haystack, const string &needle) {
	return haystack.find(needle) != string::npos;
}

void Basic() {
	auto p = Parse("<?xml version=\"1.0\"?>\n<!-- a comment -->\n<configuration>\n"
	               "  <property><name>fs.defaultFS</name><value>hdfs://nn:8020</value></property>\n"
	               "  <!-- between properties -->\n"
	               "  <property>\n    <name>dfs.replication</name>\n    <value>2</value>\n  </property>\n"
	               "</configuration>\n");
	CHECK(p.size() == 2);
	CHECK(p["fs.defaultFS"] == "hdfs://nn:8020");
	CHECK(p["dfs.replication"] == "2");
}

// Later files override earlier ones; keys only in the earlier file stay.
void Override() {
	map<string, string> p;
	HdfsParseHadoopXml("<configuration><property><name>a</name><value>core</value></property>"
	                   "<property><name>b</name><value>core</value></property></configuration>",
	                   "core-site.xml", p);
	HdfsParseHadoopXml("<configuration><property><name>a</name><value>hdfs</value></property></configuration>",
	                   "hdfs-site.xml", p);
	CHECK(p["a"] == "hdfs");
	CHECK(p["b"] == "core");
}

// libhdfs3's readConfigItem, case by case.
void LibHdfs3Semantics() {
	// A name without a value is an empty value; so is an empty <value/>.
	auto p = Parse("<configuration><property><name>a</name></property>"
	               "<property><name>b</name><value/></property></configuration>");
	CHECK(p.count("a") && p["a"].empty());
	CHECK(p.count("b") && p["b"].empty());
	// Text is not trimmed, and nothing is interpreted: no ${...} substitution, <final> and <description> ignored.
	p = Parse("<configuration><property><name>k</name><value> ${user.name} </value><final>true</final>"
	          "<description>d</description></property></configuration>");
	CHECK(p["k"] == " ${user.name} ");
	// The first <name> / <value> with text counts; later ones are ignored. Order within a property doesn't matter.
	p = Parse("<configuration><property><value>v1</value><name>k</name><value>v2</value><name>other</name>"
	          "</property></configuration>");
	CHECK(p.size() == 1 && p["k"] == "v1");
	// A <name> without text doesn't count, so a later one can.
	p = Parse("<configuration><property><name/><name>k</name><value>v</value></property></configuration>");
	CHECK(p.size() == 1 && p["k"] == "v");
	// Only a text node as first child counts: CDATA or a comment first gives no value.
	p = Parse("<configuration><property><name>c</name><value><![CDATA[x]]></value></property>"
	          "<property><name>d</name><value><!-- c -->y</value></property></configuration>");
	CHECK(p["c"].empty());
	CHECK(p["d"].empty());
	// Entities are never expanded: a reference first gives no value, and an external one reads nothing.
	p = Parse("<?xml version=\"1.0\"?><!DOCTYPE configuration [<!ENTITY e SYSTEM \"file:///etc/hostname\">]>"
	          "<configuration><property><name>e</name><value>&e;</value></property></configuration>");
	CHECK(p.count("e") && p["e"].empty());
	// Character references are part of the text.
	p = Parse("<configuration><property><name>amp</name><value>a&amp;b</value></property></configuration>");
	CHECK(p["amp"] == "a&b");
	// An empty <configuration> sets nothing.
	CHECK(Parse("<configuration/>").empty());
}

void Errors() {
	CHECK(Contains(ParseError("<settings/>"), "root element is not <configuration>"));
	CHECK(Contains(ParseError("<configuration><setting/></configuration>"), "contains <setting>"));
	CHECK(Contains(ParseError("<configuration><property><value>v</value></property></configuration>"),
	               "a <property> has no <name>"));
	CHECK(Contains(ParseError("<configuration><property>"), "test.xml"));
	CHECK(Contains(ParseError(""), "can't be read"));
}

//! Parse `xml` with both parsers: both must fail, or both succeed with the same value for every key ours found.
//! (libhdfs3's Config can't list its keys, so keys only it found would go unnoticed; a missing key reads as unset.)
void CheckParity(const string &xml) {
	char path[] = "/tmp/hdfs_hadoop_xml_testXXXXXX";
	int fd = mkstemp(path);
	if (fd < 0 || write(fd, xml.data(), xml.size()) != static_cast<ssize_t>(xml.size())) {
		std::fprintf(stderr, "can't write a temporary file\n");
		failures++;
		return;
	}
	close(fd);
	bool libhdfs3_failed = false;
	Hdfs::Config config;
	try {
		config.update(path);
	} catch (...) {
		libhdfs3_failed = true;
	}
	unlink(path);
	map<string, string> ours;
	bool ours_failed = false;
	try {
		HdfsParseHadoopXml(xml, path, ours);
	} catch (const InvalidInputException &) {
		ours_failed = true;
	}
	if (ours_failed != libhdfs3_failed) {
		std::fprintf(stderr, "parity: ours %s, libhdfs3 %s, for: %s\n", ours_failed ? "failed" : "succeeded",
		             libhdfs3_failed ? "failed" : "succeeded", xml.c_str());
		failures++;
		return;
	}
	for (auto &property : ours) {
		const char *theirs = config.getString(property.first.c_str(), nullptr);
		if (!theirs || property.second != theirs) {
			std::fprintf(stderr, "parity: '%s' is '%s' for us, %s%s%s for libhdfs3, in: %s\n", property.first.c_str(),
			             property.second.c_str(), theirs ? "'" : "", theirs ? theirs : "unset", theirs ? "'" : "",
			             xml.c_str());
			failures++;
		}
	}
}

void Parity() {
	const char *const cases[] = {
	    "<configuration><property><name>fs.defaultFS</name><value>hdfs://nn:8020</value></property></configuration>",
	    "<?xml version=\"1.0\"?><!-- c --><configuration>\n <!-- c --> <property>\n <name>a</name>\n "
	    "<value>1</value>\n </property></configuration>",
	    "<configuration><property><name>a</name></property><property><name>b</name><value/></property>"
	    "</configuration>",
	    "<configuration><property><name>k</name><value> ${user.name} </value><final>true</final></property>"
	    "</configuration>",
	    "<configuration><property><value>v1</value><name>k</name><value>v2</value><name>x</name></property>"
	    "</configuration>",
	    "<configuration><property><name/><name>k</name><value>v</value></property></configuration>",
	    "<configuration><property><name>c</name><value><![CDATA[x]]></value></property>"
	    "<property><name>d</name><value><!-- c -->y</value></property></configuration>",
	    "<?xml version=\"1.0\"?><!DOCTYPE configuration [<!ENTITY e SYSTEM \"file:///etc/hostname\">]>"
	    "<configuration><property><name>e</name><value>&e;</value></property></configuration>",
	    "<?xml version=\"1.0\"?><!DOCTYPE configuration [<!ENTITY i \"internal\">]>"
	    "<configuration><property><name>i</name><value>&i;</value></property></configuration>",
	    "<configuration><property><name>amp</name><value>a&amp;b&#65;</value></property></configuration>",
	    "<configuration><property><name>a</name><value>x</value></property>"
	    "<property><name>a</name><value>y</value></property></configuration>",
	    "<configuration/>",
	    "<settings/>",
	    "<configuration><setting/></configuration>",
	    "<configuration><property><value>v</value></property></configuration>",
	    "<configuration><property>",
	    "",
	};
	for (auto xml : cases) {
		CheckParity(xml);
	}
}

} // namespace

int main() {
	Parity();
	Basic();
	Override();
	LibHdfs3Semantics();
	Errors();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("All Hadoop XML tests passed\n");
	return 0;
}
