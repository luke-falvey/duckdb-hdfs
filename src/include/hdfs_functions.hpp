#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class HdfsFileSystem;

//! Register hdfs_ls, hdfs_stat and hdfs_exists. `hdfs` is owned by the database's VirtualFileSystem and outlives
//! the functions.
void RegisterHdfsFunctions(ExtensionLoader &loader, HdfsFileSystem &hdfs);

} // namespace duckdb
