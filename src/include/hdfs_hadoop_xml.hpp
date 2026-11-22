//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_hadoop_xml.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/map.hpp"

namespace duckdb {

//! Parse a Hadoop configuration file (core-site.xml / hdfs-site.xml) into `properties`, overwriting keys already there
//! (so later files override earlier ones), exactly as libhdfs3 reads one (XmlConfig.cpp readConfigItems): the root
//! must be <configuration> and its elements <property>; a property's name and value are the text of its first <name>
//! and first <value> whose first child is a text node, not trimmed; a name without a value is "". Nothing else is
//! interpreted: no ${...} substitution, <final>, XInclude, entities or DTD. The parse never touches the network or
//! other files, so a file can't pull local content into a secret. Throws InvalidInputException naming `path`.
void HdfsParseHadoopXml(const string &xml, const string &path, map<string, string> &properties);

} // namespace duckdb
