//===----------------------------------------------------------------------===//
//                         DuckDB
//
// hdfs_glob.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

//! Whether an HDFS path is a glob pattern: it contains `*`, `?`, `[` or `{`. Unlike FileSystem::HasGlob, this
//! includes `{` (brace alternation). Scans and hdfs_ls both use it, so they agree on what a pattern is; as in
//! DuckDB's globber, escapes don't suppress glob mode.
bool IsHdfsGlob(const string &path);

//! Glob patterns for HDFS directory walks: DuckDB's syntax (`*`, `?`, `[abc]`, `[a-b]`, `[!abc]`, `\` escapes, and
//! `**` as a whole component matching zero or more levels, at most once per pattern), plus Hadoop-style `{a,b}`
//! alternation. Components are matched with DuckDB's own glob matcher.
//!
//! Hidden names (a leading `.` or `_`, e.g. `_temporary`) are skipped unless the plan includes them, or a component
//! spells the hidden character out (a literal, or a pattern starting with a possibly escaped `.`/`_`, like `_*`);
//! `**` neither matches nor descends into them.
//!
//! Brace groups expand into a set of patterns (at most MAX_EXPANDED_PATTERNS), and matching runs as a small state
//! machine over directory levels: the walk carries a set of states per directory and calls Match for each child.
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
	//! The most patterns brace expansion may produce: each group multiplies the count, so a few dozen groups would
	//! otherwise exhaust memory.
	static constexpr idx_t MAX_EXPANDED_PATTERNS = 10000;
	//! The most brace groups (`{` that open a group, nested or not) a pattern may have: expansion recurses once per
	//! group, and a group with one alternative never counts towards MAX_EXPANDED_PATTERNS.
	static constexpr idx_t MAX_BRACE_GROUPS = 128;
	//! The longest pattern accepted, in bytes (the NameNode's own path limit is 8000 characters; brace alternatives
	//! make a pattern longer than any path it expands to).
	static constexpr idx_t MAX_PATTERN_LENGTH = 65536;
	//! The most bytes brace expansion may build, counting every intermediate pattern: the length and group caps
	//! alone allow a 64 KiB pattern with 13 groups (8,192 copies of 64 KiB, about 0.5 GB outside DuckDB's memory
	//! limit).
	static constexpr idx_t MAX_EXPANSION_BYTES = 16ULL * 1024 * 1024;

	//! Throws InvalidInputException for an invalid pattern (unclosed brace group, more than one `**`, more than
	//! MAX_EXPANDED_PATTERNS expanded patterns, more than MAX_BRACE_GROUPS groups, longer than MAX_PATTERN_LENGTH,
	//! more than MAX_EXPANSION_BYTES expanded, or a literal `.` or `..` component after expansion and unescaping).
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

	//! At most the first 200 bytes of `pattern` (with "..." if cut), for error messages: a pattern may be 64 KiB.
	static string Excerpt(const string &pattern);
	//! Hadoop's hidden-name convention: a leading `.` or `_`.
	static bool IsHidden(const string &name);
	//! Expand `{a,b}` groups into brace-free patterns. Braces inside a character class (`[{]`) or escaped (`\{`) are
	//! literal. Throws InvalidInputException for an unclosed group, more than MAX_EXPANDED_PATTERNS results, more than
	//! MAX_BRACE_GROUPS groups, a pattern longer than MAX_PATTERN_LENGTH or more than MAX_EXPANSION_BYTES built.
	static vector<string> ExpandBraces(const string &pattern);

private:
	enum class ComponentKind { CRAWL, LITERAL, PATTERN };
	struct Component {
		ComponentKind kind;
		//! LITERAL: the unescaped text. PATTERN: the raw pattern.
		string text;
	};

	struct StateHash {
		size_t operator()(const State &state) const;
	};
	//! The states already in a Step's `next`, so adding one is O(1) however many patterns braces produced.
	using StateSet = unordered_set<State, StateHash>;

	void MatchAt(State state, const string &name, bool is_dir, Step &step, StateSet &seen) const;
	static void ExpandBraces(const string &pattern, const string &original, vector<string> &result, idx_t &budget);

	vector<vector<Component>> patterns;
	string root;
	vector<State> initial;
	bool emit_root = false;
	bool include_hidden = false;
};

} // namespace duckdb
