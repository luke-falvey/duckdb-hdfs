// Unit tests for the HDFS glob planner (src/hdfs_glob.cpp): brace expansion, roots, hidden-name rules and the `**`
// state machine. No cluster needed. Build and run with `make test_glob`.

#include "hdfs_glob.hpp"

#include "duckdb/common/exception.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace duckdb;
using Strings = std::vector<std::string>;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

#define CHECK_EQ(actual, expected)                                                                                     \
	do {                                                                                                               \
		auto actual_value = (actual);                                                                                  \
		auto expected_value = (expected);                                                                              \
		if (!(actual_value == expected_value)) {                                                                       \
			std::fprintf(stderr, "%s:%d: CHECK_EQ failed: %s\n", __FILE__, __LINE__, #actual);                         \
			failures++;                                                                                                \
		}                                                                                                              \
	} while (0)

//! Whether `fn` throws an InvalidInputException whose message contains `needle`.
template <class FN>
bool Throws(FN &&fn, const std::string &needle = "") {
	try {
		fn();
	} catch (const InvalidInputException &error) {
		return std::string(error.what()).find(needle) != std::string::npos;
	}
	return false;
}

// A directory tree: path -> children as (name, is_dir).
using Tree = std::map<std::string, std::vector<std::pair<std::string, bool>>>;

// A sequential stand-in for the real walk (src/hdfs_client.cpp), collecting emitted paths. The real walk stats the
// root when the plan emits it; here the root is emitted if the tree knows it (as a directory or a file).
Strings RunWalk(const HdfsGlobPlan &plan, const Tree &tree) {
	Strings emitted;
	if (plan.EmitRoot()) {
		bool known = tree.count(plan.Root()) > 0;
		for (auto &dir : tree) {
			for (auto &child : dir.second) {
				known |= (dir.first == "/" ? "/" : dir.first + "/") + child.first == plan.Root();
			}
		}
		if (known) {
			emitted.push_back(plan.Root());
		}
	}
	std::vector<std::pair<std::string, duckdb::vector<HdfsGlobPlan::State>>> queue;
	if (!plan.Initial().empty()) {
		queue.emplace_back(plan.Root(), plan.Initial());
	}
	while (!queue.empty()) {
		auto item = queue.back();
		queue.pop_back();
		auto it = tree.find(item.first);
		if (it == tree.end()) {
			continue;
		}
		for (auto &child : it->second) {
			std::string path = item.first == "/" ? "/" + child.first : item.first + "/" + child.first;
			auto step = plan.Match(item.second, child.first, child.second);
			if (step.emit) {
				emitted.push_back(path);
			}
			if (child.second && !step.next.empty()) {
				queue.emplace_back(path, step.next);
			}
		}
	}
	std::sort(emitted.begin(), emitted.end());
	return emitted;
}

Strings Walk(const std::string &pattern, const Tree &tree, bool include_hidden = false) {
	return RunWalk(HdfsGlobPlan::Parse(pattern, include_hidden), tree);
}

void GlobDetection() {
	CHECK(!IsHdfsGlob("/a/b.csv"));
	CHECK(IsHdfsGlob("/a/*.csv"));
	CHECK(IsHdfsGlob("/a/?.csv"));
	CHECK(IsHdfsGlob("/a/data[1].csv"));
	// Unlike FileSystem::HasGlob, a brace is a glob character: brace expansion treats it as one.
	CHECK(IsHdfsGlob("/a/a{b}.csv"));
	CHECK(IsHdfsGlob("/a/a{b.csv"));
	CHECK(!IsHdfsGlob("/a/b}.csv"));
}

void ComponentMatching() {
	// Each component is matched with DuckDB's Glob(), through the plan.
	Tree tree = {{"/d",
	              {{"file.txt", false},
	               {"file.jpg", false},
	               {"ac", false},
	               {"ab", false},
	               {"abc", false},
	               {"data1.csv", false},
	               {"data12.csv", false},
	               {"data.csv", false},
	               {"data\xc3\xa9.csv", false},
	               {"a.txt", false},
	               {"b.txt", false},
	               {"d.txt", false},
	               {"file3.txt", false},
	               {"a-b", false},
	               {"]", false},
	               {"file*name", false},
	               {"fileXname", false},
	               {"file[", false}}}};
	CHECK_EQ(Walk("/d/*.txt", tree), (Strings {"/d/a.txt", "/d/b.txt", "/d/d.txt", "/d/file.txt", "/d/file3.txt"}));
	CHECK_EQ(Walk("/d/a**c", tree), (Strings {"/d/abc", "/d/ac"})); // '**' inside a component is '*'
	CHECK_EQ(Walk("/d/a*c", tree), (Strings {"/d/abc", "/d/ac"}));
	CHECK_EQ(Walk("/d/data?.csv", tree), (Strings {"/d/data1.csv"}));
	// '?' matches one byte, as for local files: a two-byte UTF-8 character needs '??'.
	CHECK_EQ(Walk("/d/data??.csv", tree), (Strings {"/d/data12.csv", "/d/data\xc3\xa9.csv"}));
	CHECK_EQ(Walk("/d/[abc].txt", tree), (Strings {"/d/a.txt", "/d/b.txt"}));
	CHECK_EQ(Walk("/d/file[0-9].txt", tree), (Strings {"/d/file3.txt"}));
	CHECK_EQ(Walk("/d/[!a].txt", tree), (Strings {"/d/b.txt", "/d/d.txt"}));
	CHECK_EQ(Walk("/d/a[x-]b", tree), (Strings {"/d/a-b"})); // a trailing '-' is literal
	CHECK_EQ(Walk("/d/[]]", tree), (Strings {"/d/]"}));      // a first ']' is literal
	// An escaped wildcard is literal: the pattern is all-literal and names the path itself.
	CHECK_EQ(Walk("/d/file\\*name", tree), (Strings {"/d/file*name"}));
	// A malformed class matches nothing (DuckDB behaviour) rather than erroring.
	CHECK(Walk("/d/file[", tree).empty());
}

void BraceExpansion() {
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/a/b"), (Strings {"/a/b"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/a/{b,c}"), (Strings {"/a/b", "/a/c"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/{a/b,c/d}/e"), (Strings {"/a/b/e", "/c/d/e"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/{a,{b,c}}"), (Strings {"/a", "/b", "/c"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/{a,b}/{c,d}"), (Strings {"/a/c", "/a/d", "/b/c", "/b/d"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/a/{}"), (Strings {"/a/"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/a/{,x}"), (Strings {"/a/", "/a/x"}));
	// Escaped braces are literal.
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/a/\\{b,c\\}"), (Strings {"/a/\\{b,c\\}"}));
	// A bare '}' is literal; an unclosed '{' is an error.
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/a/b}"), (Strings {"/a/b}"}));
	CHECK(Throws([] { HdfsGlobPlan::ExpandBraces("/a/{b,c"); }, "Unclosed brace group in glob pattern '/a/{b,c'"));
	CHECK(Throws([] { HdfsGlobPlan::ExpandBraces("/{a,{b}/c"); }, "Unclosed brace group"));
}

void BraceCharacterClasses() {
	// Braces and commas inside a character class are class members, not alternation.
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/[{]x"), (Strings {"/d/[{]x"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/[}{]x"), (Strings {"/d/[}{]x"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/{a,[,]b}"), (Strings {"/d/a", "/d/[,]b"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/{[}]x,y}"), (Strings {"/d/[}]x", "/d/y"}));
	// A ']' first in the class (or after '!') is a member, so the class runs to the next ']'.
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/[]{]"), (Strings {"/d/[]{]"}));
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/[!]{]"), (Strings {"/d/[!]{]"}));
	// A '[' with no closing ']' is not a class, so a brace after it still groups.
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/[{a,b}"), (Strings {"/d/[a", "/d/[b"}));
	// Escapes inside a class are not escapes (as in DuckDB's Glob): '\' is a member and ']' closes the class.
	CHECK_EQ(HdfsGlobPlan::ExpandBraces("/d/[\\]{a,b}"), (Strings {"/d/[\\]a", "/d/[\\]b"}));

	Tree tree = {{"/d", {{"{x", false}, {"ax", false}, {"a", false}, {",b", false}, {"bb", false}}}};
	CHECK_EQ(Walk("/d/[{]x", tree), (Strings {"/d/{x"}));
	CHECK_EQ(Walk("/d/{a,[,]b}", tree), (Strings {"/d/,b", "/d/a"}));
}

void BraceExpansionCap() {
	// Each group multiplies the count: 13 groups of two make 8192 patterns, 14 make 16384.
	std::string pattern = "/d";
	for (int group = 0; group < 13; group++) {
		pattern += "/{a,b}";
	}
	CHECK_EQ(HdfsGlobPlan::ExpandBraces(pattern).size(), size_t(8192));
	auto plan = HdfsGlobPlan::Parse(pattern, false);
	CHECK_EQ(plan.Initial().size(), size_t(8192));
	auto too_many = pattern + "/{a,b}";
	CHECK(Throws([&] { HdfsGlobPlan::ExpandBraces(too_many); }, "expands to more than 10000 patterns"));
	CHECK(Throws([&] { HdfsGlobPlan::Parse(too_many, false); }, "expands to more than 10000 patterns"));
	// A pattern that would expand to 2^40 patterns fails fast instead of exhausting memory.
	std::string huge = "/d/";
	for (int group = 0; group < 40; group++) {
		huge += "{a,b}";
	}
	CHECK(Throws([&] { HdfsGlobPlan::Parse(huge, false); }, "expands to more than 10000 patterns"));
	// Exactly the cap is allowed.
	std::string alternatives = "/d/{";
	for (int alternative = 0; alternative < 10000; alternative++) {
		alternatives += (alternative ? "," : "") + std::to_string(alternative);
	}
	alternatives += "}";
	CHECK_EQ(HdfsGlobPlan::ExpandBraces(alternatives).size(), size_t(10000));
	CHECK(Throws([&] { HdfsGlobPlan::ExpandBraces(alternatives + "{x,y}"); }, "expands to more than 10000"));
}

void ManyPatternsDedupe() {
	// 1000 alternatives that all match the same directory: the child gets each state once, and the walk emits each
	// file once.
	std::string pattern = "/d/{";
	for (int alternative = 0; alternative < 1000; alternative++) {
		pattern += (alternative ? ",*" : "*") + std::to_string(alternative % 10) + "*";
	}
	pattern += "}/*.csv";
	auto plan = HdfsGlobPlan::Parse(pattern, false);
	CHECK_EQ(plan.Initial().size(), size_t(1000));
	auto step = plan.Match(plan.Initial(), "x0123456789", true);
	CHECK_EQ(step.next.size(), size_t(1000));
	auto again = plan.Match(step.next, "f.csv", false);
	CHECK(again.emit);
	Tree tree = {{"/d", {{"x0123456789", true}}}, {"/d/x0123456789", {{"f.csv", false}}}};
	CHECK_EQ(RunWalk(plan, tree), (Strings {"/d/x0123456789/f.csv"}));
}

void PlanRoots() {
	auto plan = HdfsGlobPlan::Parse("/warehouse/db/table/*.parquet", false);
	CHECK_EQ(plan.Root(), std::string("/warehouse/db/table"));
	CHECK_EQ(plan.Initial().size(), size_t(1));
	CHECK(!plan.EmitRoot());

	// Escaped wildcards make an all-literal pattern: the root matches.
	plan = HdfsGlobPlan::Parse("/a/file\\*name", false);
	CHECK_EQ(plan.Root(), std::string("/a/file*name"));
	CHECK(plan.Initial().empty());
	CHECK(plan.EmitRoot());

	// Brace alternatives share only "/data": that's the root.
	plan = HdfsGlobPlan::Parse("/data/{2024/01,2025/02}/part-*", false);
	CHECK_EQ(plan.Root(), std::string("/data"));
	CHECK_EQ(plan.Initial().size(), size_t(2));

	// One all-literal alternative emits the root, the other walks below it.
	plan = HdfsGlobPlan::Parse("/data{,/*}", false);
	CHECK_EQ(plan.Root(), std::string("/data"));
	CHECK(plan.EmitRoot());
	CHECK_EQ(plan.Initial().size(), size_t(1));

	plan = HdfsGlobPlan::Parse("/*", false);
	CHECK_EQ(plan.Root(), std::string("/"));

	CHECK(Throws([] { HdfsGlobPlan::Parse("/a/**/b/**", false); }, "Cannot use multiple '**' in one path"));
	CHECK(Throws([] { HdfsGlobPlan::Parse("/a/{b,c", false); }, "Unclosed brace group"));
}

void WalkStarAndClasses() {
	Tree tree = {
	    {"/data", {{"a.csv", false}, {"b.txt", false}, {"sub", true}}},
	    {"/data/sub", {{"c.csv", false}}},
	};
	CHECK_EQ(Walk("/data/*.csv", tree), (Strings {"/data/a.csv"}));
	// '*' matches directories too, but only emits (never lists) them.
	CHECK_EQ(Walk("/data/*", tree), (Strings {"/data/a.csv", "/data/b.txt", "/data/sub"}));
	CHECK_EQ(Walk("/data/[ab].*", tree), (Strings {"/data/a.csv", "/data/b.txt"}));
	CHECK_EQ(Walk("/data/*/*.csv", tree), (Strings {"/data/sub/c.csv"}));
	CHECK(Walk("/data/zzz*", tree).empty());
}

void WalkCrawl() {
	Tree tree = {
	    {"/d", {{"x.parquet", false}, {"a", true}}},
	    {"/d/a", {{"y.parquet", false}, {"b", true}}},
	    {"/d/a/b", {{"z.parquet", false}}},
	};
	// A terminal '**' emits everything at every depth, directories included.
	CHECK_EQ(Walk("/d/**", tree), (Strings {"/d/a", "/d/a/b", "/d/a/b/z.parquet", "/d/a/y.parquet", "/d/x.parquet"}));
	// '**' also matches zero levels (DuckDB semantics).
	CHECK_EQ(Walk("/d/**/*.parquet", tree), (Strings {"/d/a/b/z.parquet", "/d/a/y.parquet", "/d/x.parquet"}));
	// A component after '**' prunes what gets emitted, not the descent.
	CHECK_EQ(Walk("/d/**/b/*.parquet", tree), (Strings {"/d/a/b/z.parquet"}));
	// '**' combined with brace alternatives: each file is emitted once.
	CHECK_EQ(Walk("/d/**/{*.parquet,z*}", tree), (Strings {"/d/a/b/z.parquet", "/d/a/y.parquet", "/d/x.parquet"}));
}

void WalkBraces() {
	Tree tree = {
	    {"/d", {{"jan", true}, {"feb", true}, {"mar", true}}},
	    {"/d/jan", {{"1.csv", false}}},
	    {"/d/feb", {{"2.csv", false}}},
	    {"/d/mar", {{"3.csv", false}}},
	};
	CHECK_EQ(Walk("/d/{jan,feb}/*.csv", tree), (Strings {"/d/feb/2.csv", "/d/jan/1.csv"}));
	// Overlapping alternatives must not emit duplicates.
	CHECK_EQ(Walk("/d/{jan,j*}/*.csv", tree), (Strings {"/d/jan/1.csv"}));
	CHECK_EQ(Walk("/d/{jan,feb,mar}/{1,2}.csv", tree), (Strings {"/d/feb/2.csv", "/d/jan/1.csv"}));
}

void WalkHidden() {
	Tree tree = {
	    {"/d", {{"a.csv", false}, {"_SUCCESS", false}, {".staged", false}, {"_temporary", true}, {"sub", true}}},
	    {"/d/_temporary", {{"part.csv", false}}},
	    {"/d/sub", {{"b.csv", false}, {"_SUCCESS", false}}},
	};
	// Wildcards skip hidden names by default.
	CHECK_EQ(Walk("/d/*", tree), (Strings {"/d/a.csv", "/d/sub"}));
	// A crawl neither emits hidden entries nor descends into hidden dirs.
	CHECK_EQ(Walk("/d/**", tree), (Strings {"/d/a.csv", "/d/sub", "/d/sub/b.csv"}));
	// include_hidden restores everything.
	CHECK_EQ(Walk("/d/*", tree, true), (Strings {"/d/.staged", "/d/_SUCCESS", "/d/_temporary", "/d/a.csv", "/d/sub"}));
	CHECK_EQ(Walk("/d/**", tree, true),
	         (Strings {"/d/.staged", "/d/_SUCCESS", "/d/_temporary", "/d/_temporary/part.csv", "/d/a.csv", "/d/sub",
	                   "/d/sub/_SUCCESS", "/d/sub/b.csv"}));
	// Explicitly naming a hidden directory still works.
	CHECK_EQ(Walk("/d/_temporary/*", tree), (Strings {"/d/_temporary/part.csv"}));
	// An explicit (or escaped) hidden first character matches hidden names; `[...]`/`?`/`*` first characters don't.
	CHECK_EQ(Walk("/d/_*", tree), (Strings {"/d/_SUCCESS", "/d/_temporary"}));
	CHECK_EQ(Walk("/d/\\_*", tree), (Strings {"/d/_SUCCESS", "/d/_temporary"}));
	CHECK_EQ(Walk("/d/[_a]*", tree), (Strings {"/d/a.csv"}));
	CHECK(Walk("/d/?SUCCESS", tree).empty());
	// An explicit literal after `**` matches hidden names via the zero-level case.
	CHECK_EQ(Walk("/d/**/_SUCCESS", tree), (Strings {"/d/_SUCCESS", "/d/sub/_SUCCESS"}));
	// A brace alternative that spells out the hidden prefix matches it.
	CHECK_EQ(Walk("/d/{_S*,a*}", tree), (Strings {"/d/_SUCCESS", "/d/a.csv"}));
}

} // namespace

int main() {
	GlobDetection();
	ComponentMatching();
	BraceExpansion();
	BraceCharacterClasses();
	BraceExpansionCap();
	ManyPatternsDedupe();
	PlanRoots();
	WalkStarAndClasses();
	WalkCrawl();
	WalkBraces();
	WalkHidden();
	if (failures) {
		std::fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	std::printf("All glob tests passed\n");
	return 0;
}
