//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_logging.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

//! The `HDFS` log type: one entry per NameNode RPC the client makes (`CALL enable_logging('HDFS')`, then
//! `duckdb_logs_parsed('HDFS')`). `path` is the HDFS path without scheme and authority; `duration_us` is the wall
//! time of the call, including any retries and failovers inside libhdfs3.
class HdfsLogType : public LogType {
public:
	static constexpr const char *NAME = "HDFS";
	static constexpr LogLevel LEVEL = LogLevel::LOG_DEBUG;

	HdfsLogType() : LogType(NAME, LEVEL, GetLogType()) {
	}

	static LogicalType GetLogType() {
		child_list_t<LogicalType> children = {{"op", LogicalType::VARCHAR},
		                                      {"path", LogicalType::VARCHAR},
		                                      {"duration_us", LogicalType::BIGINT},
		                                      {"ok", LogicalType::BOOLEAN}};
		return LogicalType::STRUCT(children);
	}

	static string ConstructLogMessage(const string &op, const string &path, int64_t duration_us, bool ok) {
		child_list_t<Value> children = {{"op", Value(op)},
		                                {"path", Value(path)},
		                                {"duration_us", Value::BIGINT(duration_us)},
		                                {"ok", Value::BOOLEAN(ok)}};
		return Value::STRUCT(std::move(children)).ToString();
	}
};

//! The logger for HDFS RPCs made on behalf of `opener`: the query's logger, else the database's (null without
//! either).
inline shared_ptr<Logger> HdfsLoggerFor(optional_ptr<FileOpener> opener) {
	auto context = FileOpener::TryGetClientContext(opener);
	if (context) {
		return context->logger;
	}
	auto db = FileOpener::TryGetDatabase(opener);
	if (db) {
		return db->GetLogManager().GlobalLoggerReference();
	}
	return nullptr;
}

} // namespace duckdb
