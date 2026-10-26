// Unit tests for the HDFS glob engine (src/hdfs_glob.cpp). No cluster needed.
// Build and run with `make test_glob`.

#include "hdfs_glob.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace hdfs_glob;
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

template <class FN>
bool Throws(FN &&fn) {
	try {
		fn();
	} catch (const GlobError &) {
		return true;
	}
	return false;
}

// A directory tree: path -> children as (name, is_dir).
using Tree = std::map<std::string, std::vector<std::pair<std::string, bool>>>;

// A sequential stand-in for the real walk, collecting emitted paths.
Strings RunWalk(const GlobPlan &plan, const Tree &tree) {
	Strings emitted;
	std::vector<std::pair<std::string, std::vector<Pos>>> queue = {{plan.Root(), plan.Initial()}};
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

void ComponentMatching() {
	CHECK(MatchComponent("file.txt", "*.txt"));
	CHECK(!MatchComponent("file.jpg", "*.txt"));
	CHECK(MatchComponent("file.txt", "*"));
	CHECK(MatchComponent("", "*"));
	CHECK(MatchComponent("abc", "a**c")); // '**' inside a component = '*'
	CHECK(MatchComponent("ac", "a*c"));
	CHECK(!MatchComponent("ab", "a*c"));

	CHECK(MatchComponent("data1.csv", "data?.csv"));
	CHECK(!MatchComponent("data12.csv", "data?.csv"));
	CHECK(!MatchComponent("data.csv", "data?.csv"));
	CHECK(MatchComponent("dataé.csv", "data?.csv")); // '?' is one character, not one byte

	CHECK(MatchComponent("a.txt", "[abc].txt"));
	CHECK(!MatchComponent("d.txt", "[abc].txt"));
	CHECK(MatchComponent("file3.txt", "file[0-9].txt"));
	CHECK(!MatchComponent("filex.txt", "file[0-9].txt"));
	CHECK(MatchComponent("b.txt", "[!a].txt"));
	CHECK(!MatchComponent("a.txt", "[!a].txt"));
	CHECK(MatchComponent("a-b", "a[x-]b")); // trailing '-' is literal
	CHECK(MatchComponent("]", "[]]"));      // first ']' is literal

	CHECK(MatchComponent("file*name", "file\\*name"));
	CHECK(!MatchComponent("fileXname", "file\\*name"));

	// Malformed patterns match nothing (DuckDB behavior), don't error.
	CHECK(!MatchComponent("file[", "file["));
	CHECK(!MatchComponent("abc", "abc\\"));
}

void BraceExpansion() {
	CHECK_EQ(ExpandBraces("/a/b"), (Strings {"/a/b"}));
	CHECK_EQ(ExpandBraces("/a/{b,c}"), (Strings {"/a/b", "/a/c"}));
	CHECK_EQ(ExpandBraces("/{a/b,c/d}/e"), (Strings {"/a/b/e", "/c/d/e"}));
	CHECK_EQ(ExpandBraces("/{a,{b,c}}"), (Strings {"/a", "/b", "/c"}));
	CHECK_EQ(ExpandBraces("/{a,b}/{c,d}"), (Strings {"/a/c", "/a/d", "/b/c", "/b/d"}));
	// Escaped braces are literal.
	CHECK_EQ(ExpandBraces("/a/\\{b,c\\}"), (Strings {"/a/\\{b,c\\}"}));
	// A bare '}' is literal; an unclosed '{' is an error.
	CHECK_EQ(ExpandBraces("/a/b}"), (Strings {"/a/b}"}));
	CHECK(Throws([] { ExpandBraces("/a/{b,c"); }));
}

void PlanRoots() {
	auto plan = GlobPlan::Parse("/warehouse/db/table/*.parquet", false);
	CHECK_EQ(plan.Root(), std::string("/warehouse/db/table"));
	CHECK_EQ(plan.Initial().size(), size_t(1));
	CHECK(!plan.EmitRoot());

	// Escaped wildcards make an all-literal pattern: the root matches.
	plan = GlobPlan::Parse("/a/file\\*name", false);
	CHECK_EQ(plan.Root(), std::string("/a/file*name"));
	CHECK(plan.Initial().empty());
	CHECK(plan.EmitRoot());

	// Brace alternatives share only "/data" -> that's the root.
	plan = GlobPlan::Parse("/data/{2024/01,2025/02}/part-*", false);
	CHECK_EQ(plan.Root(), std::string("/data"));
	CHECK_EQ(plan.Initial().size(), size_t(2));

	plan = GlobPlan::Parse("/*", false);
	CHECK_EQ(plan.Root(), std::string("/"));

	CHECK(Throws([] { GlobPlan::Parse("/a/**/b/**", false); })); // multiple '**'
}

void WalkStarAndClasses() {
	Tree tree = {
	    {"/data", {{"a.csv", false}, {"b.txt", false}, {"sub", true}}},
	    {"/data/sub", {{"c.csv", false}}},
	};
	CHECK_EQ(RunWalk(GlobPlan::Parse("/data/*.csv", false), tree), (Strings {"/data/a.csv"}));
	// '*' matches directories too, but only emits (never lists) them.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/data/*", false), tree), (Strings {"/data/a.csv", "/data/b.txt", "/data/sub"}));
	CHECK_EQ(RunWalk(GlobPlan::Parse("/data/[ab].*", false), tree), (Strings {"/data/a.csv", "/data/b.txt"}));
	CHECK_EQ(RunWalk(GlobPlan::Parse("/data/*/*.csv", false), tree), (Strings {"/data/sub/c.csv"}));
	CHECK(RunWalk(GlobPlan::Parse("/data/zzz*", false), tree).empty());
}

void WalkCrawl() {
	Tree tree = {
	    {"/d", {{"x.parquet", false}, {"a", true}}},
	    {"/d/a", {{"y.parquet", false}, {"b", true}}},
	    {"/d/a/b", {{"z.parquet", false}}},
	};
	// Terminal '**' emits everything at every depth, directories included.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/**", false), tree),
	         (Strings {"/d/a", "/d/a/b", "/d/a/b/z.parquet", "/d/a/y.parquet", "/d/x.parquet"}));
	// '**' also matches zero levels (DuckDB semantics).
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/**/*.parquet", false), tree),
	         (Strings {"/d/a/b/z.parquet", "/d/a/y.parquet", "/d/x.parquet"}));
	// A component after '**' prunes what gets emitted, not the descent.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/**/b/*.parquet", false), tree), (Strings {"/d/a/b/z.parquet"}));
}

void WalkBraces() {
	Tree tree = {
	    {"/d", {{"jan", true}, {"feb", true}, {"mar", true}}},
	    {"/d/jan", {{"1.csv", false}}},
	    {"/d/feb", {{"2.csv", false}}},
	    {"/d/mar", {{"3.csv", false}}},
	};
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/{jan,feb}/*.csv", false), tree), (Strings {"/d/feb/2.csv", "/d/jan/1.csv"}));
	// Overlapping alternatives must not emit duplicates.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/{jan,j*}/*.csv", false), tree), (Strings {"/d/jan/1.csv"}));
}

void WalkHidden() {
	Tree tree = {
	    {"/d", {{"a.csv", false}, {"_SUCCESS", false}, {".staged", false}, {"_temporary", true}, {"sub", true}}},
	    {"/d/_temporary", {{"part.csv", false}}},
	    {"/d/sub", {{"b.csv", false}, {"_SUCCESS", false}}},
	};
	// Wildcards skip hidden names by default.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/*", false), tree), (Strings {"/d/a.csv", "/d/sub"}));
	// A crawl neither emits hidden entries nor descends into hidden dirs.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/**", false), tree), (Strings {"/d/a.csv", "/d/sub", "/d/sub/b.csv"}));
	// include_hidden restores everything.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/*", true), tree),
	         (Strings {"/d/.staged", "/d/_SUCCESS", "/d/_temporary", "/d/a.csv", "/d/sub"}));
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/**", true), tree),
	         (Strings {"/d/.staged", "/d/_SUCCESS", "/d/_temporary", "/d/_temporary/part.csv", "/d/a.csv", "/d/sub",
	                   "/d/sub/_SUCCESS", "/d/sub/b.csv"}));
	// Explicitly naming a hidden directory still works.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/_temporary/*", false), tree), (Strings {"/d/_temporary/part.csv"}));
	// An explicit (or escaped) hidden first character matches hidden names;
	// `[...]`/`?`/`*` first characters don't.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/_*", false), tree), (Strings {"/d/_SUCCESS", "/d/_temporary"}));
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/\\_*", false), tree), (Strings {"/d/_SUCCESS", "/d/_temporary"}));
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/[_a]*", false), tree), (Strings {"/d/a.csv"}));
	CHECK(RunWalk(GlobPlan::Parse("/d/?SUCCESS", false), tree).empty());
	// An explicit literal after `**` matches hidden names via the zero-level defer.
	CHECK_EQ(RunWalk(GlobPlan::Parse("/d/**/_SUCCESS", false), tree), (Strings {"/d/_SUCCESS", "/d/sub/_SUCCESS"}));
}

} // namespace

int main() {
	ComponentMatching();
	BraceExpansion();
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
