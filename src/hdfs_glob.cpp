#include "hdfs_glob.hpp"

#include <algorithm>

namespace hdfs_glob {

namespace {

// Decode UTF-8 into code points so `?` and character classes match one
// character rather than one byte. Invalid bytes decode as themselves.
std::u32string Decode(const std::string &s) {
	std::u32string out;
	out.reserve(s.size());
	size_t i = 0;
	while (i < s.size()) {
		auto c = static_cast<unsigned char>(s[i]);
		size_t len = 1;
		char32_t cp = c;
		if (c >= 0xF0 && c < 0xF8) {
			len = 4;
			cp = c & 0x07;
		} else if (c >= 0xE0) {
			len = 3;
			cp = c & 0x0F;
		} else if (c >= 0xC0) {
			len = 2;
			cp = c & 0x1F;
		}
		if (len > 1 && i + len <= s.size()) {
			bool valid = true;
			for (size_t k = 1; k < len; k++) {
				auto cc = static_cast<unsigned char>(s[i + k]);
				if ((cc & 0xC0) != 0x80) {
					valid = false;
					break;
				}
				cp = (cp << 6) | (cc & 0x3F);
			}
			if (valid) {
				out.push_back(cp);
				i += len;
				continue;
			}
		}
		out.push_back(c);
		i++;
	}
	return out;
}

// Match `c` against the character class starting at p[pi] (just past the
// `[`). Returns the index after the closing `]` on a match, or npos when the
// class doesn't match or is unclosed.
size_t MatchClass(char32_t c, const std::u32string &p, size_t pi) {
	bool invert = false;
	if (pi < p.size() && p[pi] == U'!') {
		invert = true;
		pi++;
	}
	size_t start = pi;
	bool found = false;
	for (;;) {
		if (pi >= p.size()) {
			return std::u32string::npos; // unclosed class
		}
		// A ']' in the first position is a literal member.
		if (p[pi] == U']' && pi > start) {
			pi++;
			break;
		}
		// A range "a-b", unless the '-' is the last char before ']'.
		if (pi + 2 < p.size() && p[pi + 1] == U'-' && p[pi + 2] != U']') {
			if (p[pi] <= c && c <= p[pi + 2]) {
				found = true;
			}
			pi += 3;
		} else {
			if (p[pi] == c) {
				found = true;
			}
			pi++;
		}
	}
	return found != invert ? pi : std::u32string::npos;
}

bool MatchChars(const std::u32string &s, size_t si, const std::u32string &p, size_t pi) {
	while (si < s.size() && pi < p.size()) {
		switch (p[pi]) {
		case U'*': {
			while (pi < p.size() && p[pi] == U'*') {
				pi++;
			}
			if (pi == p.size()) {
				return true; // trailing '*' matches the rest
			}
			// Backtrack: try the remaining pattern at every suffix.
			for (size_t start = si; start < s.size(); start++) {
				if (MatchChars(s, start, p, pi)) {
					return true;
				}
			}
			return false;
		}
		case U'?':
			si++;
			pi++;
			break;
		case U'[': {
			size_t after = MatchClass(s[si], p, pi + 1);
			if (after == std::u32string::npos) {
				return false;
			}
			si++;
			pi = after;
			break;
		}
		case U'\\':
			pi++;
			if (pi == p.size() || s[si] != p[pi]) {
				return false;
			}
			si++;
			pi++;
			break;
		default:
			if (s[si] != p[pi]) {
				return false;
			}
			si++;
			pi++;
			break;
		}
	}
	// Consume trailing '*'s; the match succeeds if both are exhausted.
	while (pi < p.size() && p[pi] == U'*') {
		pi++;
	}
	return si == s.size() && pi == p.size();
}

// Whether a raw pattern component explicitly names a hidden first character:
// a (possibly escaped) literal leading `.` or `_`.
bool ExplicitHiddenPrefix(const std::string &pattern) {
	if (pattern.empty()) {
		return false;
	}
	if (pattern[0] == '\\') {
		return pattern.size() > 1 && (pattern[1] == '.' || pattern[1] == '_');
	}
	return pattern[0] == '.' || pattern[0] == '_';
}

void PushState(std::vector<Pos> &states, Pos pos) {
	if (std::find(states.begin(), states.end(), pos) == states.end()) {
		states.push_back(pos);
	}
}

std::vector<std::string> SplitComponents(const std::string &path) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= path.size()) {
		size_t slash = path.find('/', start);
		if (slash == std::string::npos) {
			slash = path.size();
		}
		if (slash > start) {
			out.push_back(path.substr(start, slash - start));
		}
		start = slash + 1;
	}
	return out;
}

} // namespace

bool IsHidden(const std::string &name) {
	return !name.empty() && (name[0] == '.' || name[0] == '_');
}

bool MatchComponent(const std::string &name, const std::string &pattern) {
	return MatchChars(Decode(name), 0, Decode(pattern), 0);
}

std::vector<std::string> ExpandBraces(const std::string &pattern) {
	// Find the first unescaped '{'.
	size_t start = std::string::npos;
	for (size_t i = 0; i < pattern.size(); i++) {
		if (pattern[i] == '\\') {
			i++; // skip the escaped character
		} else if (pattern[i] == '{') {
			start = i;
			break;
		}
	}
	if (start == std::string::npos) {
		return {pattern};
	}

	// Collect its top-level alternatives up to the matching '}'.
	std::vector<std::string> alternatives;
	std::string current;
	size_t close = std::string::npos;
	int depth = 1;
	for (size_t j = start + 1; j < pattern.size(); j++) {
		char c = pattern[j];
		if (c == '\\') {
			current.push_back(c);
			if (j + 1 < pattern.size()) {
				current.push_back(pattern[++j]);
			}
		} else if (c == '{') {
			depth++;
			current.push_back(c);
		} else if (c == '}') {
			if (--depth == 0) {
				alternatives.push_back(current);
				current.clear();
				close = j;
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
	if (close == std::string::npos) {
		throw GlobError("Unclosed brace group in glob pattern '" + pattern + "'");
	}

	std::string prefix = pattern.substr(0, start);
	std::string suffix = pattern.substr(close + 1);
	std::vector<std::string> expanded;
	for (auto &alt : alternatives) {
		// Each level removes one brace pair, so the recursion terminates.
		for (auto &e : ExpandBraces(prefix + alt + suffix)) {
			expanded.push_back(std::move(e));
		}
	}
	return expanded;
}

GlobPlan GlobPlan::Parse(const std::string &pattern, bool include_hidden) {
	GlobPlan plan;
	plan.include_hidden = include_hidden;
	for (auto &expanded : ExpandBraces(pattern)) {
		std::vector<Component> components;
		size_t crawls = 0;
		for (auto &raw : SplitComponents(expanded)) {
			Component comp;
			if (raw == "**") {
				comp.kind = Kind::CRAWL;
				crawls++;
			} else {
				comp.kind = Kind::LITERAL;
				for (size_t i = 0; i < raw.size(); i++) {
					char c = raw[i];
					if (c == '\\') {
						// A dangling escape stays literal.
						comp.text.push_back(i + 1 < raw.size() ? raw[++i] : '\\');
					} else if (c == '*' || c == '?' || c == '[') {
						comp.kind = Kind::PATTERN;
						comp.text = raw;
						break;
					} else {
						comp.text.push_back(c);
					}
				}
			}
			components.push_back(std::move(comp));
		}
		if (crawls > 1) {
			// Same restriction (and message) as DuckDB's globber.
			throw GlobError("Cannot use multiple '**' in one path: '" + pattern + "'");
		}
		plan.patterns.push_back(std::move(components));
	}

	// The longest literal prefix shared by all patterns is the walk root.
	size_t consumed = 0;
	std::string root;
	for (;;) {
		auto &first_pattern = plan.patterns[0];
		if (consumed >= first_pattern.size() || first_pattern[consumed].kind != Kind::LITERAL) {
			break;
		}
		const std::string &first = first_pattern[consumed].text;
		bool shared = true;
		for (size_t i = 1; i < plan.patterns.size(); i++) {
			auto &p = plan.patterns[i];
			if (consumed >= p.size() || p[consumed].kind != Kind::LITERAL || p[consumed].text != first) {
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

	// A pattern fully consumed by the prefix matches the root itself.
	for (size_t pat = 0; pat < plan.patterns.size(); pat++) {
		if (plan.patterns[pat].size() == consumed) {
			plan.emit_root = true;
		} else {
			plan.initial.push_back(Pos {pat, consumed});
		}
	}
	return plan;
}

Step GlobPlan::Match(const std::vector<Pos> &states, const std::string &name, bool is_dir) const {
	Step step;
	// A crawl never consumes a hidden level. The zero-level defer still runs,
	// so an explicit component after `**` (e.g. `**/_SUCCESS`) can match.
	bool crawl_blocked = !include_hidden && IsHidden(name);
	for (auto pos : states) {
		auto &components = patterns[pos.pat];
		if (components[pos.comp].kind == Kind::CRAWL) {
			if (pos.comp + 1 == components.size()) {
				if (!crawl_blocked) {
					step.emit = true; // terminal `**`: everything matches
				}
			} else {
				// Match zero levels, deferring to the component after it.
				MatchAt(Pos {pos.pat, pos.comp + 1}, name, is_dir, step);
			}
			if (is_dir && !crawl_blocked) {
				PushState(step.next, pos); // keep crawling below
			}
		} else {
			MatchAt(pos, name, is_dir, step);
		}
	}
	return step;
}

void GlobPlan::MatchAt(Pos pos, const std::string &name, bool is_dir, Step &step) const {
	auto &components = patterns[pos.pat];
	auto &comp = components[pos.comp];
	bool matched;
	if (comp.kind == Kind::LITERAL) {
		// A matching literal spells any hidden prefix out itself.
		matched = name == comp.text;
	} else {
		// Parse() rejects `**/**`, and Match() never advances into a crawl here.
		matched =
		    (include_hidden || !IsHidden(name) || ExplicitHiddenPrefix(comp.text)) && MatchComponent(name, comp.text);
	}
	if (!matched) {
		return;
	}
	if (pos.comp + 1 == components.size()) {
		step.emit = true;
	} else if (is_dir) {
		PushState(step.next, Pos {pos.pat, pos.comp + 1});
	}
}

} // namespace hdfs_glob
