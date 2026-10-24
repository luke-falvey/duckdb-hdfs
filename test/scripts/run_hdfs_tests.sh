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
# standard Hadoop env vars that hdfs-native reads natively.
HDFS_TEST_RUNNING=1 \
HADOOP_CONF_DIR="${REPO_ROOT}/test/hdfs-conf" \
HADOOP_USER_NAME=hadoop \
    "${UNITTEST}" test/sql/hdfs.test

echo "==> Running HDFS permission tests (as a non-superuser)..."
# A separate run as a plain user: the superuser above bypasses HDFS permission
# checks, so it can never hit the AccessControlExceptions these tests need.
HDFS_TEST_RUNNING=1 \
HADOOP_CONF_DIR="${REPO_ROOT}/test/hdfs-conf" \
HADOOP_USER_NAME=guest \
    "${UNITTEST}" test/sql/hdfs_permissions.test

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
HDFS_TEST_RUNNING=1 HDFS_TEST_NO_KMS=1 \
HADOOP_CONF_DIR="${NO_KMS_CONF}" \
HADOOP_USER_NAME=hadoop \
    "${UNITTEST}" test/sql/hdfs_ez_no_provider.test
rm -rf "${NO_KMS_CONF}"

