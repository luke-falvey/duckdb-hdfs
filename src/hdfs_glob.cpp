#include "hdfs_glob.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar/string_common.hpp"

#include <algorithm>

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

void AddState(vector<HdfsGlobPlan::State> &states, HdfsGlobPlan::State state) {
	if (std::find(states.begin(), states.end(), state) == states.end()) {
		states.push_back(state);
	}
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

bool HdfsGlobPlan::IsHidden(const string &name) {
	return !name.empty() && (name[0] == '.' || name[0] == '_');
}

vector<string> HdfsGlobPlan::ExpandBraces(const string &pattern) {
	// The first unescaped '{'.
	auto start = string::npos;
	for (idx_t char_idx = 0; char_idx < pattern.size(); char_idx++) {
		if (pattern[char_idx] == '\\') {
			char_idx++;
		} else if (pattern[char_idx] == '{') {
			start = char_idx;
			break;
		}
	}
	if (start == string::npos) {
		return {pattern};
	}
	// Its top-level alternatives, up to the matching '}'.
	vector<string> alternatives;
	string current;
	auto close = string::npos;
	idx_t depth = 1;
	for (idx_t char_idx = start + 1; char_idx < pattern.size(); char_idx++) {
		auto c = pattern[char_idx];
		if (c == '\\') {
			current.push_back(c);
			if (char_idx + 1 < pattern.size()) {
				current.push_back(pattern[++char_idx]);
			}
		} else if (c == '{') {
			depth++;
			current.push_back(c);
		} else if (c == '}') {
			if (--depth == 0) {
				alternatives.push_back(current);
				close = char_idx;
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
		throw InvalidInputException("Unclosed brace group in glob pattern '%s'", pattern);
	}
	auto prefix = pattern.substr(0, start);
	auto suffix = pattern.substr(close + 1);
	vector<string> expanded;
	for (auto &alternative : alternatives) {
		// Each level removes one brace pair, so the recursion terminates.
		for (auto &result : ExpandBraces(prefix + alternative + suffix)) {
			expanded.push_back(std::move(result));
		}
	}
	return expanded;
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
			}
			components.push_back(std::move(component));
		}
		if (crawls > 1) {
			// The same restriction (and message) as DuckDB's globber.
			throw InvalidInputException("Cannot use multiple '**' in one path: '%s'", pattern);
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
	for (auto state : states) {
		auto &components = patterns[state.pattern_idx];
		if (components[state.comp].kind != ComponentKind::CRAWL) {
			MatchAt(state, name, is_dir, step);
			continue;
		}
		if (state.comp + 1 == components.size()) {
			// A terminal `**` matches everything.
			step.emit |= !crawl_blocked;
		} else {
			// Zero levels: match the component after the `**`.
			MatchAt(State {state.pattern_idx, state.comp + 1}, name, is_dir, step);
		}
		if (is_dir && !crawl_blocked) {
			AddState(step.next, state);
		}
	}
	return step;
}

void HdfsGlobPlan::MatchAt(State state, const string &name, bool is_dir, Step &step) const {
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
		AddState(step.next, State {state.pattern_idx, state.comp + 1});
	}
}

} // namespace duckdb
