#include "hdfs_hadoop_xml.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <cstring>
#include <memory>

namespace duckdb {

namespace {

[[noreturn]] void ThrowBadFile(const string &path, const string &why) {
	throw InvalidInputException("Hadoop configuration file '%s' can't be read: %s", path, why);
}

bool IsElement(xmlNodePtr node, const char *name) {
	return node->type == XML_ELEMENT_NODE && std::strcmp(reinterpret_cast<const char *>(node->name), name) == 0;
}

//! The text of a <name> or <value> element, as libhdfs3 takes it: its first child's content, if that child is a text
//! node (so CDATA, comments or a nested element first don't count).
bool FirstText(xmlNodePtr node, string &out) {
	if (!node->children || node->children->type != XML_TEXT_NODE || !node->children->content) {
		return false;
	}
	out = reinterpret_cast<const char *>(node->children->content);
	return true;
}

} // namespace

void HdfsParseHadoopXml(const string &xml, const string &path, map<string, string> &properties) {
	// No XML_PARSE_NOENT (entities stay unexpanded), no XML_PARSE_DTDLOAD or DTDATTR (no external DTD), and
	// XML_PARSE_NONET: nothing outside the given bytes is ever read. XInclude only happens on request
	// (xmlXIncludeProcess), which this never makes. Errors are reported through the exception, not stderr.
	const int options = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
	std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
	    xmlReadMemory(xml.data(), NumericCast<int>(xml.size()), path.c_str(), nullptr, options), xmlFreeDoc);
	if (!doc) {
		auto error = xmlGetLastError();
		ThrowBadFile(path, error && error->message ? StringUtil::Replace(string(error->message), "\n", "")
		                                           : string("not well-formed XML"));
	}
	auto root = xmlDocGetRootElement(doc.get());
	if (!root || !IsElement(root, "configuration")) {
		ThrowBadFile(path, "its root element is not <configuration>");
	}
	for (auto property = root->children; property; property = property->next) {
		if (property->type != XML_ELEMENT_NODE) {
			continue;
		}
		if (!IsElement(property, "property")) {
			ThrowBadFile(path, StringUtil::Format("<configuration> contains <%s>, not only <property> elements",
			                                      reinterpret_cast<const char *>(property->name)));
		}
		string key;
		string value;
		bool has_name = false;
		bool has_value = false;
		for (auto child = property->children; child; child = child->next) {
			if (!has_name && IsElement(child, "name")) {
				has_name = FirstText(child, key);
			} else if (!has_value && IsElement(child, "value")) {
				has_value = FirstText(child, value);
			}
		}
		if (!has_name) {
			ThrowBadFile(path, "a <property> has no <name>");
		}
		properties[key] = has_value ? value : string();
	}
}

} // namespace duckdb
