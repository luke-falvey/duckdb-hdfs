#include "hdfs_functions.hpp"

#include "hdfs_filesystem.hpp"
#include "hdfs_glob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

//! Which call an hdfs_ls / hdfs_stat invocation makes; both share one schema and execution path.
enum class HdfsMetaOp { LIST, STAT };

struct HdfsTableFunctionInfo : public TableFunctionInfo {
	HdfsTableFunctionInfo(HdfsFileSystem &hdfs, HdfsMetaOp op) : hdfs(hdfs), op(op) {
	}

	reference<HdfsFileSystem> hdfs;
	HdfsMetaOp op;
};

struct HdfsScalarFunctionInfo : public ScalarFunctionInfo {
	explicit HdfsScalarFunctionInfo(HdfsFileSystem &hdfs) : hdfs(hdfs) {
	}

	reference<HdfsFileSystem> hdfs;
};

//! Column layout of hdfs_ls / hdfs_stat.
constexpr idx_t COL_PATH = 0;
constexpr idx_t COL_NAME = 1;
constexpr idx_t COL_TYPE = 2;
constexpr idx_t COL_SIZE = 3;
constexpr idx_t COL_OWNER = 4;
constexpr idx_t COL_GROUP = 5;
constexpr idx_t COL_PERMISSIONS = 6;
constexpr idx_t COL_MODE = 7;
constexpr idx_t COL_REPLICATION = 8;
constexpr idx_t COL_BLOCK_SIZE = 9;
constexpr idx_t COL_LAST_MODIFIED = 10;
constexpr idx_t COL_LAST_ACCESSED = 11;

void DefineMetadataSchema(vector<LogicalType> &types, vector<Identifier> &names) {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,      LogicalType::BIGINT,
	         LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,      LogicalType::USMALLINT,
	         LogicalType::INTEGER, LogicalType::BIGINT,  LogicalType::TIMESTAMP_TZ, LogicalType::TIMESTAMP_TZ};
	names = {"path",        "name", "type",        "size",       "owner",         "group",
	         "permissions", "mode", "replication", "block_size", "last_modified", "last_accessed"};
}

//! The 9 permission bits as "rwxr-xr-x", as `hdfs dfs -ls` shows them.
string PermissionString(uint16_t mode) {
	static const char *const RWX[] = {"---", "--x", "-w-", "-wx", "r--", "r-x", "rw-", "rwx"};
	return string(RWX[(mode >> 6) & 0x7]) + RWX[(mode >> 3) & 0x7] + RWX[mode & 0x7];
}

//! The checks DuckDB's OpenerFileSystem makes before every file system call: the file system isn't in
//! `disabled_filesystems`, and with `enable_external_access = false` the path is in `allowed_directories` /
//! `allowed_paths`. The functions call HdfsFileSystem directly, so they must make them themselves.
void VerifyCanAccess(ClientContext &context, HdfsFileSystem &hdfs, const string &path) {
	// By name rather than by path: a scheme-less path (the default FS) would otherwise resolve to the local FS.
	if (FileSystem::GetFileSystem(context).SubSystemIsDisabled(hdfs.GetName())) {
		throw PermissionException("File system %s has been disabled by configuration", hdfs.GetName());
	}
	auto &config = DBConfig::GetConfig(context);
	auto opener = ClientData::Get(context).file_opener.get();
	// The path may name a file or a directory (an allowed directory "hdfs://nn/d/" allows "hdfs://nn/d" itself).
	if (!config.CanAccessFile(path, FileType::FILE_TYPE_REGULAR, opener) &&
	    !config.CanAccessFile(path, FileType::FILE_TYPE_DIR, opener)) {
		throw PermissionException("Cannot access path \"%s\" - file system operations are disabled by configuration",
		                          path);
	}
}

struct HdfsMetaBindData : public TableFunctionData {
	HdfsMetaBindData(HdfsFileSystem &hdfs, HdfsMetaOp op, string path) : hdfs(hdfs), op(op), path(std::move(path)) {
	}

	reference<HdfsFileSystem> hdfs;
	HdfsMetaOp op;
	string path;
	//! Per-call overrides of the hdfs_skip_permission_errors / hdfs_include_hidden settings; NULL if not given.
	Value skip_permission_errors;
	Value include_hidden;
};

unique_ptr<FunctionData> HdfsMetaBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &info = input.info->Cast<HdfsTableFunctionInfo>();
	if (input.inputs[0].IsNull()) {
		throw BinderException("%s: the path cannot be NULL", info.op == HdfsMetaOp::LIST ? "hdfs_ls" : "hdfs_stat");
	}
	auto result = make_uniq<HdfsMetaBindData>(info.hdfs.get(), info.op, StringValue::Get(input.inputs[0]));
	// Only hdfs_ls has named parameters.
	for (auto &parameter : input.named_parameters) {
		if (parameter.first == "skip_permission_errors") {
			result->skip_permission_errors = parameter.second;
		} else if (parameter.first == "include_hidden") {
			result->include_hidden = parameter.second;
		}
	}
	DefineMetadataSchema(return_types, names);
	return std::move(result);
}

//! LIST streams batches from a background walk; STAT runs its RPC once in Init and holds the row.
struct HdfsMetaGlobalState : public GlobalTableFunctionState {
	vector<column_t> column_ids;
	unique_ptr<HdfsListStream> stream;
	//! A glob that has matched nothing yet; if it ends that way, the path is tried as a literal path.
	bool literal_fallback = false;
	HdfsWalkOptions options;
	vector<HdfsEntry> entries;
	idx_t offset = 0;
	//! Rows emitted so far (the next row id).
	idx_t rows_emitted = 0;
};

//! An extension setting's current value. Read through the client's FileOpener rather than
//! ClientContext::TryGetCurrentSetting: the latter references the table of all built-in settings, which pulls the
//! SQL parser and more into the binary (about 1.5 MB).
bool TryGetSetting(ClientContext &context, const char *name, Value &value) {
	return FileOpener::TryGetCurrentSetting(ClientData::Get(context).file_opener.get(), name, value) && !value.IsNull();
}

//! A walk flag: the per-call parameter wins; otherwise the setting, or `fallback` if it isn't set.
bool WalkFlag(ClientContext &context, const Value &override_value, const char *setting_name, bool fallback) {
	if (!override_value.IsNull()) {
		return BooleanValue::Get(override_value);
	}
	Value setting;
	return TryGetSetting(context, setting_name, setting) ? BooleanValue::Get(setting) : fallback;
}

unique_ptr<GlobalTableFunctionState> HdfsMetaInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<HdfsMetaBindData>();
	auto &hdfs = bind_data.hdfs.get();
	auto opener = ClientData::Get(context).file_opener.get();
	// In Init rather than bind, so a prepared statement is checked again when the settings change.
	VerifyCanAccess(context, hdfs, bind_data.path);
	auto state = make_uniq<HdfsMetaGlobalState>();
	state->column_ids = input.column_ids;
	if (bind_data.op == HdfsMetaOp::STAT) {
		state->entries.push_back(hdfs.Stat(bind_data.path, opener));
		return std::move(state);
	}
	auto &options = state->options;
	options.skip_permission_errors =
	    WalkFlag(context, bind_data.skip_permission_errors, "hdfs_skip_permission_errors", false);
	options.include_hidden = WalkFlag(context, bind_data.include_hidden, "hdfs_include_hidden", false);
	options.batched_listing = WalkFlag(context, Value(), "hdfs_batched_listing", true);
	options.max_parallelism = DEFAULT_HDFS_LIST_PARALLELISM;
	Value parallelism;
	if (TryGetSetting(context, "hdfs_list_parallelism", parallelism)) {
		options.max_parallelism = UBigIntValue::Get(parallelism);
	}
	// A path with glob characters returns the matched entries themselves; a literal path lists the directory's
	// children. As in DuckDB's globber, escapes don't suppress glob mode (`\*` then matches a literal `*`).
	if (IsHdfsGlob(bind_data.path)) {
		state->stream = hdfs.OpenGlobStream(bind_data.path, options, opener);
		state->literal_fallback = true;
	} else {
		state->stream = hdfs.OpenListStream(bind_data.path, options, opener);
	}
	return std::move(state);
}

//! Write column `column_id` of `rows` into `result`; row ids start at `first_row_id`.
void WriteColumn(column_t column_id, const vector<HdfsEntry> &rows, idx_t first_row_id, Vector &result) {
	auto count = rows.size();
	if (column_id == COLUMN_IDENTIFIER_ROW_ID) {
		auto writer = FlatVector::Writer<int64_t>(result, count);
		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			writer.WriteValue(NumericCast<int64_t>(first_row_id + row_idx));
		}
		return;
	}
	switch (column_id) {
	case COL_PATH:
	case COL_NAME:
	case COL_TYPE:
	case COL_OWNER:
	case COL_GROUP:
	case COL_PERMISSIONS: {
		auto writer = FlatVector::Writer<string_t>(result, count);
		for (auto &row : rows) {
			switch (column_id) {
			case COL_PATH:
				writer.WriteValue(string_t(row.url));
				break;
			case COL_NAME:
				writer.WriteValue(string_t(row.name));
				break;
			case COL_TYPE:
				writer.WriteValue(string_t(row.is_dir ? "directory" : "file"));
				break;
			case COL_OWNER:
				writer.WriteValue(string_t(row.owner));
				break;
			case COL_GROUP:
				writer.WriteValue(string_t(row.group));
				break;
			default: {
				auto permissions = PermissionString(row.permission);
				writer.WriteValue(string_t(permissions));
				break;
			}
			}
		}
		break;
	}
	case COL_SIZE:
	case COL_BLOCK_SIZE: {
		auto writer = FlatVector::Writer<int64_t>(result, count);
		for (auto &row : rows) {
			auto value = column_id == COL_SIZE ? row.size : row.block_size;
			// HDFS has no block size for directories: -1 becomes NULL.
			if (value < 0 && column_id == COL_BLOCK_SIZE) {
				writer.WriteNull();
			} else {
				writer.WriteValue(value);
			}
		}
		break;
	}
	case COL_MODE: {
		auto writer = FlatVector::Writer<uint16_t>(result, count);
		for (auto &row : rows) {
			writer.WriteValue(row.permission);
		}
		break;
	}
	case COL_REPLICATION: {
		auto writer = FlatVector::Writer<int32_t>(result, count);
		for (auto &row : rows) {
			if (row.replication < 0) {
				writer.WriteNull();
			} else {
				writer.WriteValue(row.replication);
			}
		}
		break;
	}
	case COL_LAST_MODIFIED:
	case COL_LAST_ACCESSED: {
		// HDFS times are UTC instants, so TIMESTAMP WITH TIME ZONE (as core's read_blob/read_text).
		auto writer = FlatVector::Writer<timestamp_tz_t>(result, count);
		for (auto &row : rows) {
			auto time = column_id == COL_LAST_MODIFIED ? row.mtime : row.atime;
			// An access time of 0 means "not tracked": always for directories, and for files on clusters with
			// dfs.namenode.accesstime.precision = 0.
			if (column_id == COL_LAST_ACCESSED && time == timestamp_t(0)) {
				writer.WriteNull();
			} else {
				writer.WriteValue(timestamp_tz_t(time.value));
			}
		}
		break;
	}
	default:
		throw InternalException("hdfs_ls: unexpected column id %llu", column_id);
	}
}

//! The next non-empty batch of `stream`, or an empty one at its end. Blocks until the walk produces rows (or ends);
//! the wait can be interrupted.
void NextBatch(ClientContext &context, HdfsListStream &stream, vector<HdfsEntry> &rows) {
	while (stream.Next(rows, STANDARD_VECTOR_SIZE, &context) && rows.empty()) {
	}
}

void HdfsMetaExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<HdfsMetaGlobalState>();
	vector<HdfsEntry> rows;
	if (state.stream) {
		NextBatch(context, *state.stream, rows);
		state.literal_fallback &= rows.empty();
		if (state.literal_fallback) {
			// As in scans (and DuckDB's local glob): a pattern that matches nothing may be a path whose name contains
			// glob characters (`data[1].csv`, `a{b}.csv`). It is then listed as a literal path.
			state.literal_fallback = false;
			auto &bind_data = data_p.bind_data->Cast<HdfsMetaBindData>();
			auto &hdfs = bind_data.hdfs.get();
			auto opener = ClientData::Get(context).file_opener.get();
			if (hdfs.Exists(bind_data.path, opener)) {
				state.stream = hdfs.OpenListStream(bind_data.path, state.options, opener);
				NextBatch(context, *state.stream, rows);
			}
		}
	} else {
		auto count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.entries.size() - state.offset);
		rows.assign(state.entries.begin() + NumericCast<int64_t>(state.offset),
		            state.entries.begin() + NumericCast<int64_t>(state.offset + count));
		state.offset += count;
	}
	for (idx_t output_idx = 0; output_idx < state.column_ids.size(); output_idx++) {
		WriteColumn(state.column_ids[output_idx], rows, state.rows_emitted, output.data[output_idx]);
	}
	state.rows_emitted += rows.size();
	output.SetChildCardinality(rows.size());
}

TableFunction MakeMetaFunction(const char *name) {
	TableFunction function(name, {LogicalType::VARCHAR}, HdfsMetaExecute, HdfsMetaBind, HdfsMetaInit);
	function.GetSignature().GetParameter(0).SetName("path");
	// Only the selected columns are built (e.g. no permission strings for `SELECT name`).
	function.projection_pushdown = true;
	return function;
}

void HdfsExistsExecute(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &hdfs = func_expr.Function().GetExtraFunctionInfo().Cast<HdfsScalarFunctionInfo>().hdfs.get();
	auto &context = state.GetContext();
	auto opener = ClientData::Get(context).file_opener.get();
	UnaryExecutor::Execute<string_t, bool>(args.data[0], result, args.size(), [&](string_t path) {
		auto url = path.GetString();
		VerifyCanAccess(context, hdfs, url);
		return hdfs.Exists(url, opener);
	});
}

FunctionDescription Describe(string description, vector<string> examples) {
	FunctionDescription result;
	result.description = std::move(description);
	result.examples = std::move(examples);
	return result;
}

} // namespace

void RegisterHdfsFunctions(ExtensionLoader &loader, HdfsFileSystem &hdfs) {
	auto ls = MakeMetaFunction("hdfs_ls");
	ls.function_info = make_shared_ptr<HdfsTableFunctionInfo>(hdfs, HdfsMetaOp::LIST);
	// Per-call overrides of the same-named hdfs_* settings. They have no default: a call that leaves one out falls
	// back to the setting.
	ls.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("skip_permission_errors", LogicalType::BOOLEAN);
		options.Add("include_hidden", LogicalType::BOOLEAN);
	});
	CreateTableFunctionInfo ls_info(std::move(ls));
	ls_info.descriptions.push_back(Describe(
	    "Lists an HDFS directory's children, or the entries a glob pattern matches, with their full file status. "
	    "Named parameters skip_permission_errors and include_hidden override the hdfs_* settings of the same name.",
	    {"hdfs_ls('hdfs://namenode:8020/data')", "hdfs_ls('hdfs://namenode:8020/data/**/*.parquet')"}));
	loader.RegisterFunction(std::move(ls_info));

	auto stat = MakeMetaFunction("hdfs_stat");
	stat.function_info = make_shared_ptr<HdfsTableFunctionInfo>(hdfs, HdfsMetaOp::STAT);
	CreateTableFunctionInfo stat_info(std::move(stat));
	stat_info.descriptions.push_back(Describe("Returns the file status of one HDFS file or directory, as one row.",
	                                          {"hdfs_stat('hdfs://namenode:8020/data/events.parquet')"}));
	loader.RegisterFunction(std::move(stat_info));

	ScalarFunction exists("hdfs_exists", {LogicalType::VARCHAR}, LogicalType::BOOLEAN, HdfsExistsExecute);
	exists.GetSignature().GetParameter(0).SetName("path");
	exists.SetExtraFunctionInfo(make_shared_ptr<HdfsScalarFunctionInfo>(hdfs));
	// The lookup can fail (permission denied, NameNode unreachable); without this, DuckDB reports a thrown error as
	// a bug in a function it assumed can't fail.
	exists.SetFallible();
	// The answer changes as files come and go, so it must not be constant-folded or kept in a prepared statement's
	// plan (as the default, CONSISTENT, allows). Within one query it is one answer per row, as for now().
	exists.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	CreateScalarFunctionInfo exists_info(std::move(exists));
	exists_info.descriptions.push_back(
	    Describe("Whether an HDFS file or directory exists. Errors other than \"not found\" (e.g. permission "
	             "denied) fail the query.",
	             {"hdfs_exists('hdfs://namenode:8020/data/_SUCCESS')"}));
	loader.RegisterFunction(std::move(exists_info));
}

} // namespace duckdb
