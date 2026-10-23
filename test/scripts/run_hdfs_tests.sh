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

