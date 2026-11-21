#include "hdfs_glob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

namespace {

//! Whether a pattern component spells out a hidden first character: a (possibly escaped) leading `.` or `_`.
bool ExplicitHiddenPrefix(const string &pattern) {
	if (pattern.empty()) {
		return false;
	}
	if (pattern[0] == '\\') {
		return pattern.size() > 1 && (pattern[1] == '.' || pattern[1] == '_');
	}
	return pattern[0] == '.' || pattern[0] == '_';
}

//! If `pattern[open]` is `[` and starts a character class, the index just past its closing `]`; otherwise npos.
//! Mirrors the bracket parsing of DuckDB's Glob(): an optional `!`, a `]` right after that is literal, `a-z`
//! ranges, and no escapes inside the class. A `[` without a closing `]` is not a class.
idx_t ClassEnd(const string &pattern, idx_t open) {
	// Without any ']' after it, no '[' starts a class; checked first, so a run of unclosed '[' costs one scan per
	// '[' (bounded by MAX_PATTERN_LENGTH), not a class parse each.
	if (pattern.find(']', open + 1) == string::npos) {
		return string::npos;
	}
	auto pos = open + 1;
	if (pos < pattern.size() && pattern[pos] == '!') {
		pos++;
	}
	auto first = pos;
	while (pos < pattern.size()) {
		if (pattern[pos] == ']' && pos > first) {
			return pos + 1;
		}
		if (pos + 1 == pattern.size()) {
			break;
		}
		if (pattern[pos + 1] == '-' && pos + 2 < pattern.size() && pattern[pos + 2] != ']') {
			pos += 3;
		} else {
			pos++;
		}
	}
	return string::npos;
}

//! Characters that ExpandBraces copies verbatim starting at `pos` (an escape pair or a whole character class), or 1
//! for an ordinary character.
idx_t VerbatimLength(const string &pattern, idx_t pos) {
	if (pattern[pos] == '\\') {
		return pos + 1 < pattern.size() ? 2 : 1;
	}
	if (pattern[pos] == '[') {
		auto end = ClassEnd(pattern, pos);
		if (end != string::npos) {
			return end - pos;
		}
	}
	return 1;
}

vector<string> SplitComponents(const string &path) {
	vector<string> result;
	idx_t start = 0;
	while (start <= path.size()) {
		auto slash = path.find('/', start);
		if (slash == string::npos) {
			slash = path.size();
		}
		if (slash > start) {
			result.push_back(path.substr(start, slash - start));
		}
		start = slash + 1;
	}
	return result;
}

} // namespace

string HdfsGlobPlan::Excerpt(const string &pattern) {
	static constexpr idx_t MAX_EXCERPT = 200;
	return pattern.size() <= MAX_EXCERPT ? pattern : pattern.substr(0, MAX_EXCERPT) + "...";
}

bool HdfsGlobPlan::IsHidden(const string &name) {
	return !name.empty() && (name[0] == '.' || name[0] == '_');
}

size_t HdfsGlobPlan::StateHash::operator()(const State &state) const {
	return CombineHash(Hash(static_cast<uint64_t>(state.pattern_idx)), Hash(static_cast<uint64_t>(state.comp)));
}

bool IsHdfsGlob(const string &path) {
	return path.find_first_of("*?[{") != string::npos;
}

vector<string> HdfsGlobPlan::ExpandBraces(const string &pattern) {
	// Bound the work before expanding: the result cap alone doesn't, since a group with one alternative ("{a}")
	// never multiplies the count, yet each group is one recursion level that copies the pattern.
	if (pattern.size() > MAX_PATTERN_LENGTH) {
		throw InvalidInputException("Glob pattern is too long (%llu bytes; at most %llu)", pattern.size(),
		                            MAX_PATTERN_LENGTH);
	}
	idx_t groups = 0;
	for (idx_t char_idx = 0; char_idx < pattern.size();) {
		if (pattern[char_idx] == '{' && ++groups > MAX_BRACE_GROUPS) {
			throw InvalidInputException("Glob pattern has more than %llu brace groups", MAX_BRACE_GROUPS);
		}
		char_idx += VerbatimLength(pattern, char_idx);
	}
	vector<string> result;
	idx_t budget = MAX_EXPANSION_BYTES;
	ExpandBraces(pattern, pattern, result, budget);
	return result;
}

void HdfsGlobPlan::ExpandBraces(const string &pattern, const string &original, vector<string> &result, idx_t &budget) {
	// The first '{' that isn't escaped or inside a character class.
	auto start = string::npos;
	for (idx_t char_idx = 0; char_idx < pattern.size();) {
		if (pattern[char_idx] == '{') {
			start = char_idx;
			break;
		}
		char_idx += VerbatimLength(pattern, char_idx);
	}
	if (start == string::npos) {
		if (result.size() >= MAX_EXPANDED_PATTERNS) {
			throw InvalidInputException("Glob pattern '%s' expands to more than %llu patterns: each {a,b} group "
			                            "multiplies the count. Use fewer brace groups or several queries",
			                            Excerpt(original), MAX_EXPANDED_PATTERNS);
		}
		result.push_back(pattern);
		return;
	}
	// Its top-level alternatives, up to the matching '}'.
	vector<string> alternatives;
	string current;
	auto close = string::npos;
	idx_t depth = 1;
	for (idx_t char_idx = start + 1; char_idx < pattern.size();) {
		auto c = pattern[char_idx];
		if (c == '\\' || c == '[') {
			auto length = VerbatimLength(pattern, char_idx);
			current.append(pattern, char_idx, length);
			char_idx += length;
			continue;
		}
		char_idx++;
		if (c == '{') {
			depth++;
			current.push_back(c);
		} else if (c == '}') {
			if (--depth == 0) {
				alternatives.push_back(current);
				close = char_idx - 1;
				break;
			}
			current.push_back(c);
		} else if (c == ',' && depth == 1) {
			alternatives.push_back(current);
			current.clear();
		} else {
			current.push_back(c);
		}
	}
	if (close == string::npos) {
		throw InvalidInputException("Unclosed brace group in glob pattern '%s'", Excerpt(original));
	}
	auto prefix = pattern.substr(0, start);
	auto suffix = pattern.substr(close + 1);
	for (auto &alternative : alternatives) {
		// Each level removes one brace pair, so the recursion is at most MAX_BRACE_GROUPS deep; the count check above
		// bounds the number of leaves, and the byte budget the copying: every pattern built on the way counts, as a
		// long pattern with many groups copies itself once per group and leaf.
		auto size = prefix.size() + alternative.size() + suffix.size();
		if (size > budget) {
			throw InvalidInputException("Glob pattern '%s' is too large to expand: its brace groups would build more "
			                            "than %llu bytes of patterns. Use fewer brace groups or a shorter pattern",
			                            Excerpt(original), MAX_EXPANSION_BYTES);
		}
		budget -= size;
		ExpandBraces(prefix + alternative + suffix, original, result, budget);
	}
}

HdfsGlobPlan HdfsGlobPlan::Parse(const string &pattern, bool include_hidden) {
	HdfsGlobPlan plan;
	plan.include_hidden = include_hidden;
	for (auto &expanded : ExpandBraces(pattern)) {
		vector<Component> components;
		idx_t crawls = 0;
		for (auto &raw : SplitComponents(expanded)) {
			Component component {ComponentKind::LITERAL, string()};
			if (raw == "**") {
				component.kind = ComponentKind::CRAWL;
				crawls++;
			} else {
				for (idx_t char_idx = 0; char_idx < raw.size(); char_idx++) {
					auto c = raw[char_idx];
					if (c == '\\') {
						// A dangling escape stays literal.
						component.text.push_back(char_idx + 1 < raw.size() ? raw[++char_idx] : '\\');
					} else if (c == '*' || c == '?' || c == '[') {
						component.kind = ComponentKind::PATTERN;
						component.text = raw;
						break;
					} else {
						component.text.push_back(c);
					}
				}
				// HDFS forbids "." and ".." as names, and DuckDB's allowed_directories check only sees the raw
				// pattern: "{..}" or "\.\." would otherwise become a ".." in the walk root after expansion.
				if (component.kind == ComponentKind::LITERAL && (component.text == "." || component.text == "..")) {
					throw InvalidInputException("Glob pattern '%s' has a '%s' component (after brace expansion and "
					                            "escapes); HDFS paths cannot contain '.' or '..'",
					                            Excerpt(pattern), component.text);
				}
			}
			components.push_back(std::move(component));
		}
		if (crawls > 1) {
			// The same restriction (and message) as DuckDB's globber.
			throw InvalidInputException("Cannot use multiple '**' in one path: '%s'", Excerpt(pattern));
		}
		plan.patterns.push_back(std::move(components));
	}

	// The longest literal prefix shared by all patterns is the walk root.
	idx_t consumed = 0;
	string root;
	for (;;) {
		auto &first_pattern = plan.patterns[0];
		if (consumed >= first_pattern.size() || first_pattern[consumed].kind != ComponentKind::LITERAL) {
			break;
		}
		auto &first = first_pattern[consumed].text;
		bool shared = true;
		for (idx_t pattern_idx = 1; pattern_idx < plan.patterns.size(); pattern_idx++) {
			auto &other = plan.patterns[pattern_idx];
			if (consumed >= other.size() || other[consumed].kind != ComponentKind::LITERAL ||
			    other[consumed].text != first) {
				shared = false;
				break;
			}
		}
		if (!shared) {
			break;
		}
		root += "/" + first;
		consumed++;
	}
	plan.root = root.empty() ? "/" : root;

	// A pattern fully consumed by the root matches the root itself.
	for (idx_t pattern_idx = 0; pattern_idx < plan.patterns.size(); pattern_idx++) {
		if (plan.patterns[pattern_idx].size() == consumed) {
			plan.emit_root = true;
		} else {
			plan.initial.push_back(State {pattern_idx, consumed});
		}
	}
	return plan;
}

HdfsGlobPlan::Step HdfsGlobPlan::Match(const vector<State> &states, const string &name, bool is_dir) const {
	Step step;
	// A crawl never enters a hidden level. Its zero-level case still runs, so an explicit component after `**`
	// (e.g. `**/_SUCCESS`) can match.
	bool crawl_blocked = !include_hidden && IsHidden(name);
	StateSet seen;
	for (auto state : states) {
		auto &components = patterns[state.pattern_idx];
		if (components[state.comp].kind != ComponentKind::CRAWL) {
			MatchAt(state, name, is_dir, step, seen);
			continue;
		}
		if (state.comp + 1 == components.size()) {
			// A terminal `**` matches everything.
			step.emit |= !crawl_blocked;
		} else {
			// Zero levels: match the component after the `**`.
			MatchAt(State {state.pattern_idx, state.comp + 1}, name, is_dir, step, seen);
		}
		if (is_dir && !crawl_blocked && seen.insert(state).second) {
			step.next.push_back(state);
		}
	}
	return step;
}

void HdfsGlobPlan::MatchAt(State state, const string &name, bool is_dir, Step &step, StateSet &seen) const {
	auto &components = patterns[state.pattern_idx];
	auto &component = components[state.comp];
	bool matched;
	if (component.kind == ComponentKind::LITERAL) {
		// A literal spells out any hidden prefix itself.
		matched = name == component.text;
	} else {
		// Parse() rejects a second `**`, so this is never a crawl.
		matched = (include_hidden || !IsHidden(name) || ExplicitHiddenPrefix(component.text)) &&
		          Glob(name.c_str(), name.size(), component.text.c_str(), component.text.size());
	}
	if (!matched) {
		return;
	}
	if (state.comp + 1 == components.size()) {
		step.emit = true;
	} else if (is_dir) {
		State next {state.pattern_idx, state.comp + 1};
		if (seen.insert(next).second) {
			step.next.push_back(next);
		}
	}
}

} // namespace duckdb
