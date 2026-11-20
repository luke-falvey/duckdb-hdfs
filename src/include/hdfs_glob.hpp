//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_glob.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"

namespace duckdb {

//! Glob patterns for HDFS directory walks: DuckDB's syntax (`*`, `?`, `[abc]`, `[a-b]`, `[!abc]`, `\` escapes, and
//! `**` as a whole component matching zero or more levels, at most once per pattern), plus Hadoop-style `{a,b}`
//! alternation. Components are matched with DuckDB's own glob matcher.
//!
//! Hidden names (a leading `.` or `_`, e.g. `_temporary`) are skipped unless the plan includes them, or a component
//! spells the hidden character out (a literal, or a pattern starting with a possibly escaped `.`/`_`, like `_*`);
//! `**` neither matches nor descends into them.
//!
//! Brace groups expand into a set of patterns, and matching runs as a small state machine over directory levels:
//! the walk carries a set of states per directory and calls Match for each child.
class HdfsGlobPlan {
public:
	//! A position in the patterns: the directory has matched components 0..comp of pattern `pattern_idx`.
	struct State {
		idx_t pattern_idx;
		idx_t comp;
		bool operator==(const State &other) const {
			return pattern_idx == other.pattern_idx && comp == other.comp;
		}
	};
	//! The result of matching one child of a directory.
	struct Step {
		//! The child is a match.
		bool emit = false;
		//! States to descend into the child with (empty: don't descend).
		vector<State> next;
	};

public:
	//! Throws InvalidInputException for an invalid pattern (unclosed brace group, more than one `**`).
	static HdfsGlobPlan Parse(const string &pattern, bool include_hidden);

	//! The literal directory the walk starts from (for an all-literal pattern: the path itself).
	const string &Root() const {
		return root;
	}
	//! States for the root's children; empty for all-literal patterns.
	const vector<State> &Initial() const {
		return initial;
	}
	//! Whether the root itself is a match (some expanded pattern is all-literal).
	bool EmitRoot() const {
		return emit_root;
	}
	//! Match a child (of a directory holding `states`) by name.
	Step Match(const vector<State> &states, const string &name, bool is_dir) const;

	//! Hadoop's hidden-name convention: a leading `.` or `_`.
	static bool IsHidden(const string &name);
	//! Expand `{a,b}` groups into brace-free patterns. Throws InvalidInputException for an unclosed group.
	static vector<string> ExpandBraces(const string &pattern);

private:
	enum class ComponentKind { CRAWL, LITERAL, PATTERN };
	struct Component {
		ComponentKind kind;
		//! LITERAL: the unescaped text. PATTERN: the raw pattern.
		string text;
	};

	void MatchAt(State state, const string &name, bool is_dir, Step &step) const;

	vector<vector<Component>> patterns;
	string root;
	vector<State> initial;
	bool emit_root = false;
	bool include_hidden = false;
};

} // namespace duckdb
