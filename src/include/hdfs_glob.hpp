#pragma once

// Glob pattern parsing and matching for streaming HDFS directory walks.
//
// The syntax follows DuckDB's globber (`*`, `?`, `[abc]`, `[a-b]`, `[!abc]`,
// `\` escapes, and `**` as a whole component matching zero or more levels)
// extended with Hadoop-style `{a,b}` brace alternation. Semantics mirror DuckDB
// where they differ from Hadoop: `**` must be an entire component (elsewhere
// consecutive `*`s collapse to one), at most one `**` per pattern, and
// malformed character classes match nothing rather than erroring.
//
// Hidden entries (names starting with `.` or `_`, e.g. `_temporary`) are
// excluded unless the plan is built with `include_hidden`. A component that
// explicitly names the hidden character (a literal, or a pattern whose first
// character is a possibly escaped `.`/`_`, like `_*`) still matches; wildcard
// first characters never do, and `**` neither matches nor descends into hidden
// directories.
//
// A pattern compiles to a GlobPlan: brace groups expand into a set of
// component lists, and matching runs as a small NFA over directory levels. The
// walk carries a set of Pos states per directory and calls GlobPlan::Step for
// each child to learn whether to emit it and which states to descend with.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace hdfs_glob {

// Raised for patterns that can't be compiled (unclosed brace group, more than
// one `**`).
class GlobError : public std::runtime_error {
public:
	explicit GlobError(const std::string &message) : std::runtime_error(message) {
	}
};

// An NFA state: the current directory has matched components 0..comp of
// expanded pattern `pat`, so its children are matched against `comp`.
struct Pos {
	size_t pat = 0;
	size_t comp = 0;
	bool operator==(const Pos &other) const {
		return pat == other.pat && comp == other.comp;
	}
};

// The result of matching one directory child against a state set.
struct Step {
	// The child is itself a match: emit it.
	bool emit = false;
	// States to descend into the child with (empty: don't descend).
	std::vector<Pos> next;
};

class GlobPlan {
public:
	// Throws GlobError for an invalid pattern.
	static GlobPlan Parse(const std::string &pattern, bool include_hidden);

	// The literal directory (or, for an all-literal pattern, path) the walk
	// starts from.
	const std::string &Root() const {
		return root;
	}
	// States for the root directory's children. Empty for all-literal patterns.
	const std::vector<Pos> &Initial() const {
		return initial;
	}
	// Whether the root itself is a match (some expanded pattern is fully literal).
	bool EmitRoot() const {
		return emit_root;
	}
	// Match one child (of a directory holding `states`) by name.
	Step Match(const std::vector<Pos> &states, const std::string &name, bool is_dir) const;

private:
	enum class Kind { CRAWL, LITERAL, PATTERN };
	struct Component {
		Kind kind;
		// LITERAL: unescaped text. PATTERN: the raw pattern.
		std::string text;
	};

	void MatchAt(Pos pos, const std::string &name, bool is_dir, Step &step) const;

	std::vector<std::vector<Component>> patterns;
	std::string root;
	std::vector<Pos> initial;
	bool emit_root = false;
	bool include_hidden = false;
};

// Hadoop-convention hidden name: starts with `.` or `_`.
bool IsHidden(const std::string &name);

// Expand `{a,b}` brace groups into brace-free patterns. Throws GlobError for an
// unclosed group.
std::vector<std::string> ExpandBraces(const std::string &pattern);

// Match a single path component against a pattern (`*`, `?`, `[...]`, `\`).
// Malformed patterns match nothing.
bool MatchComponent(const std::string &name, const std::string &pattern);

} // namespace hdfs_glob
