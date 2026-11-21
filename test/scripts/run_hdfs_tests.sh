#!/usr/bin/env bash
# One-shot: bring HDFS up, run the integration SQL tests against it, tear down.
#
# Usage: test/scripts/run_hdfs_tests.sh
#
# Requires a prior `make` so build/release/test/unittest exists.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UNITTEST="${REPO_ROOT}/build/release/test/unittest"

if [ ! -x "${UNITTEST}" ]; then
    echo "ERROR: ${UNITTEST} not found. Run 'make' first." >&2
    exit 1
fi

cleanup() {
    "${REPO_ROOT}/test/scripts/hdfs_down.sh" || true
}
trap cleanup EXIT

# Small NameNode listing limits, so every run pages through listings and
# batched listings (dfs.ls.limit entries per call), and the client's first
# batched call (100 paths) is rejected and retried with the NameNode's limit.
export HDFS_SITE_EXTRA="dfs.ls.limit=5,dfs.batched.ls.limit=4"
"${REPO_ROOT}/test/scripts/hdfs_up.sh"

echo "==> Running HDFS integration tests..."
# unittest matches its registered test names by repo-relative path, so run from
# the repo root and pass the relative path.
cd "${REPO_ROOT}"
# HADOOP_CONF_DIR provides client config; HADOOP_USER_NAME makes us the owner of
# the hadoop-owned /test dir so the write tests are permitted. Both are the
# standard Hadoop env vars the extension reads, as the Hadoop CLI does.
# Every test file run here is recorded, so one that needs the cluster but is
# never run fails the script (see the end) instead of being skipped silently.
# A file skipped by an unmet `require` (an extension, or an env var such as
# HDFS_TEST_NO_KMS) exits 0 too, so a reported skip fails the script as well.
# unittest's skip summary; the self-check below fails the script if it no longer matches what unittest prints.
SKIP_MARKER="Skipped tests for the following reasons"
RAN_TESTS=""
run_cluster_test() { # test file, HADOOP_USER_NAME, [extra env assignments...]
    local test_file="$1" user="$2" output status=0
    shift 2
    output="$(mktemp)"
    env HDFS_TEST_RUNNING=1 HADOOP_CONF_DIR="${HADOOP_CONF_DIR_FOR_TESTS:-${REPO_ROOT}/test/hdfs-conf}" \
        HADOOP_USER_NAME="${user}" "$@" "${UNITTEST}" "${test_file}" 2>&1 | tee "${output}" || status=$?
    if [ "${status}" -eq 0 ] && grep -q "${SKIP_MARKER}" "${output}"; then
        echo "ERROR: ${test_file} was skipped (in whole or in part); see the reasons above" >&2
        status=1
    fi
    rm -f "${output}"
    if [ "${status}" -ne 0 ]; then
        exit "${status}"
    fi
    RAN_TESTS="${RAN_TESTS} ${test_file}"
}
# The skip check must be able to fail: a file whose require-env isn't set has to produce the marker.
skip_check="$(env -u HDFS_KERBEROS_NO_TICKET "${UNITTEST}" test/sql/hdfs_kerberos_no_ticket.test 2>&1 || true)"
if ! grep -q "${SKIP_MARKER}" <<< "${skip_check}"; then
    echo "ERROR: unittest no longer reports a skipped test as '${SKIP_MARKER}'; update the skip check" >&2
    exit 1
fi
run_cluster_test test/sql/hdfs.test hadoop
run_cluster_test test/sql/hdfs_glob.test hadoop
run_cluster_test test/sql/hdfs_edge.test hadoop
# The NameNode RPC budget of reads and listings (HDFS log type).
run_cluster_test test/sql/hdfs_rpc_log.test hadoop

echo "==> Running HDFS permission tests (as a non-superuser)..."
# A separate run as a plain user: the superuser above bypasses HDFS permission
# checks, so it can never hit the AccessControlExceptions these tests need.
run_cluster_test test/sql/hdfs_permissions.test guest

echo "==> Checking a same-size replacement with the same mtime isn't served from the external file cache..."
# hdfs dfs -cp replaces the file (a new inode) with one of the same length, and
# -touch sets the old mtime back (whole seconds, so the original is set to one
# first; in UTC, so the expected time below doesn't depend on the container's
# zone). Only the file id tells the two versions apart. Both reads run in one
# process, so the second one could hit the first one's cache entries.
# The script checks its own preconditions: the cache holds the first read,
# and both versions have the same size and mtime. A control read with
# validation off must then see the stale cached value, which shows the "2"
# comes from the version tag and not from a read the cache never served.
# .shell passes arguments with spaces unquoted, so it runs small scripts.
VERSION_TAG_DIR="$(mktemp -d)"
cat > "${VERSION_TAG_DIR}/touch.sh" <<'SH'
docker exec -e HADOOP_CLIENT_OPTS=-Duser.timezone=UTC duckdb-hdfs-test bash -lc \
    'hdfs dfs -touch -m -t 20200101:000000 /test/version_tag.parquet' >/dev/null 2>&1 || echo "touch failed"
SH
cat > "${VERSION_TAG_DIR}/replace.sh" <<'SH'
# $1: the version to copy over /test/version_tag.parquet.
docker exec -e HADOOP_CLIENT_OPTS=-Duser.timezone=UTC duckdb-hdfs-test bash -lc \
    "hdfs dfs -cp -f /test/$1 /test/version_tag.parquet &&
    hdfs dfs -touch -m -t 20200101:000000 /test/version_tag.parquet" >/dev/null 2>&1 || echo "replace failed"
SH
version_tag_out="$(HADOOP_CONF_DIR="${REPO_ROOT}/test/hdfs-conf" HADOOP_USER_NAME=hadoop \
    "${REPO_ROOT}/build/release/duckdb" -no-agent -bail -csv -noheader <<SQL
COPY (SELECT 1 AS v) TO 'hdfs://localhost:9000/test/version_tag.parquet' (FORMAT parquet);
COPY (SELECT 1 AS v) TO 'hdfs://localhost:9000/test/version_tag_1.parquet' (FORMAT parquet);
COPY (SELECT 2 AS v) TO 'hdfs://localhost:9000/test/version_tag_2.parquet' (FORMAT parquet);
SELECT 'sizes', count(DISTINCT size) FROM hdfs_ls('hdfs://localhost:9000/test/version_tag*.parquet');
.shell bash ${VERSION_TAG_DIR}/touch.sh
SELECT 'mtime', last_modified = TIMESTAMPTZ '2020-01-01 00:00:00+00' FROM hdfs_stat('hdfs://localhost:9000/test/version_tag.parquet');
SET enable_external_file_cache = true;
SET cache_local_files = true;
SELECT 'read', v FROM read_parquet('hdfs://localhost:9000/test/version_tag.parquet');
SELECT 'cached', count(*) > 0 FROM duckdb_external_file_cache() WHERE path LIKE '%/test/version_tag.parquet';
.shell bash ${VERSION_TAG_DIR}/replace.sh version_tag_2.parquet
SELECT 'mtime', last_modified = TIMESTAMPTZ '2020-01-01 00:00:00+00' FROM hdfs_stat('hdfs://localhost:9000/test/version_tag.parquet');
SELECT 'read', v FROM read_parquet('hdfs://localhost:9000/test/version_tag.parquet');
SET validate_external_file_cache = 'NO_VALIDATION';
.shell bash ${VERSION_TAG_DIR}/replace.sh version_tag_1.parquet
SELECT 'control', v FROM read_parquet('hdfs://localhost:9000/test/version_tag.parquet');
SQL
)"
rm -rf "${VERSION_TAG_DIR}"
version_tag_expected="sizes,1
mtime,true
read,1
cached,true
mtime,true
read,2
control,2"
if [ "$(echo "${version_tag_out}" | tr -d '\r')" != "${version_tag_expected}" ]; then
    echo "ERROR: unexpected output of the version-tag check (the stale-cache control reads 2, validation off)." >&2
    echo "expected:" >&2
    echo "${version_tag_expected}" >&2
    echo "got:" >&2
    echo "${version_tag_out}" >&2
    exit 1
fi
echo "OK"

echo "==> Checking batched and one-directory-per-call listings agree..."
NO_BATCH_CONF="$(mktemp -d)"
cp "${REPO_ROOT}"/test/hdfs-conf/*.xml "${NO_BATCH_CONF}/"
sed -i.bak 's#</configuration>#<property><name>duckdb.hdfs.batched-listing.enabled</name><value>false</value></property></configuration>#' \
    "${NO_BATCH_CONF}/hdfs-site.xml"
LISTINGS="
    SELECT path, type, size FROM hdfs_ls('hdfs://localhost:9000/**') ORDER BY path;
    SELECT path FROM hdfs_ls('hdfs://localhost:9000/**', include_hidden := true) ORDER BY path;
    SELECT path FROM hdfs_ls('hdfs://localhost:9000/*/*.csv') ORDER BY path;
    SELECT file FROM glob('hdfs://localhost:9000/**/*.parquet') ORDER BY file;"
# NameNode RPC counts: "<getBatchedListing calls> <getListing calls>". JMX
# values are cached for ~10 s, so wait that out before each snapshot.
listing_ops() {
    sleep 11
    curl -fsS 'http://localhost:9870/jmx?qry=Hadoop:service=NameNode,name=RpcDetailedActivityForPort9000' |
        python3 -I -c 'import json,sys; b=json.load(sys.stdin)["beans"][0]; print(b.get("GetBatchedListingNumOps", 0), b.get("GetListingNumOps", 0))'
}
run_listings() { # conf_dir
    HADOOP_CONF_DIR="$1" HADOOP_USER_NAME=hadoop "${REPO_ROOT}/build/release/duckdb" -no-agent -csv -c "${LISTINGS}"
}
read -r b0 l0 <<< "$(listing_ops)"
batched="$(run_listings "${REPO_ROOT}/test/hdfs-conf")"
read -r b1 l1 <<< "$(listing_ops)"
single="$(run_listings "${NO_BATCH_CONF}")"
read -r b2 l2 <<< "$(listing_ops)"
rm -rf "${NO_BATCH_CONF}"
if [ "${batched}" != "${single}" ]; then
    echo "ERROR: batched listings differ from one-directory-per-call listings" >&2
    diff <(echo "${batched}") <(echo "${single}") >&2 || true
    exit 1
fi
echo "OK ($(echo "${batched}" | wc -l | tr -d ' ') rows)"
echo "   batched:       $((b1 - b0)) getBatchedListing + $((l1 - l0)) getListing calls"
echo "   not batched:   $((b2 - b1)) getBatchedListing + $((l2 - l1)) getListing calls"
if [ $((b1 - b0)) -eq 0 ] || [ $((b2 - b1)) -ne 0 ] ||
    [ $(((b1 - b0) + (l1 - l0))) -ge $((l2 - l1)) ]; then
    echo "ERROR: expected batched listings to be used, and to make fewer calls" >&2
    exit 1
fi

echo "==> Checking the Java client reads what DuckDB encrypted..."
# hdfs.test writes /ez/tmp.csv inside the encryption zone.
expected="$(docker exec duckdb-hdfs-test bash -c "printf 'v\\n2\\n' | md5sum" | cut -d' ' -f1)"
actual="$(docker exec duckdb-hdfs-test bash -lc 'rm -f /tmp/ez_check.csv && hdfs dfs -get /ez/tmp.csv /tmp/ez_check.csv >/dev/null 2>&1 && md5sum /tmp/ez_check.csv' | cut -d' ' -f1)"
if [ "${actual}" != "${expected}" ]; then
    echo "ERROR: the Java client decrypted /ez/tmp.csv differently (md5 ${actual}, expected ${expected})" >&2
    exit 1
fi
echo "OK"

echo "==> Checking the Java client reads DuckDB's multi-block encrypted writes..."
# hdfs_edge.test writes the same bytes in and outside the zone, across 1 MiB blocks.
# Compared through files: `hdfs dfs -cat` on a zone file can print the Java client's crypto-library warnings
# on stdout, which would end up in a checksum of its output.
multi_block_failed=0
for f in rows.csv 2097153.bin 1048576.bin 0.bin; do
    if ! docker exec duckdb-hdfs-test bash -lc "
            set -e
            rm -f /tmp/mb_plain /tmp/mb_enc
            hdfs dfs -get /edge/duck/${f} /tmp/mb_plain >/dev/null 2>&1
            hdfs dfs -get /ez/edge/duck/${f} /tmp/mb_enc >/dev/null 2>&1
            cmp /tmp/mb_plain /tmp/mb_enc"; then
        echo "ERROR: the Java client decrypted /ez/edge/duck/${f} differently from the plain copy" >&2
        docker exec duckdb-hdfs-test bash -lc "
            echo \"  sizes read by Java: \$(stat -c %s /tmp/mb_plain 2>/dev/null) (plain), \$(stat -c %s /tmp/mb_enc 2>/dev/null) (zone)\"
            echo \"  first differing bytes (offset, plain, zone):\"
            cmp -l /tmp/mb_plain /tmp/mb_enc 2>&1 | head -5
        " >&2 || true
        multi_block_failed=1
    fi
done
if [ "${multi_block_failed}" -ne 0 ]; then
    exit 1
fi
echo "OK"

echo "==> Running encryption-zone tests without a key provider..."
NO_KMS_CONF="$(mktemp -d)"
cp "${REPO_ROOT}"/test/hdfs-conf/*.xml "${NO_KMS_CONF}/"
python3 -I - "${NO_KMS_CONF}/core-site.xml" <<'PY'
import re, sys
path = sys.argv[1]
text = open(path).read()
text = re.sub(r"\s*<property>\s*<name>hadoop\.security\.key\.provider\.path</name>.*?</property>", "", text, flags=re.S)
open(path, "w").write(text)
PY
HADOOP_CONF_DIR_FOR_TESTS="${NO_KMS_CONF}" run_cluster_test test/sql/hdfs_ez_no_provider.test hadoop HDFS_TEST_NO_KMS=1
rm -rf "${NO_KMS_CONF}"


echo "==> Checking every cluster test file ran..."
missing=""
for test_file in $(grep -l '^require-env HDFS_TEST_RUNNING' test/sql/*.test); do
    case " ${RAN_TESTS} " in
    *" ${test_file} "*) ;;
    *) missing="${missing} ${test_file}" ;;
    esac
done
if [ -n "${missing}" ]; then
    echo "ERROR: these need the cluster (require-env HDFS_TEST_RUNNING) but this script doesn't run them:${missing}" >&2
    exit 1
fi
echo "OK"
