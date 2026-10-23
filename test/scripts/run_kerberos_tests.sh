#!/usr/bin/env bash
# One-shot: start a KDC and a Kerberos-secured HDFS (test/docker-kerberos), log
# in as hadoop@EXAMPLE.TEST, run the Kerberos SQL tests, tear down.
#
# Usage: test/scripts/run_kerberos_tests.sh   (needs Docker and a prior `make`)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UNITTEST="${REPO_ROOT}/build/release/test/unittest"
DUCKDB="${REPO_ROOT}/build/release/duckdb"
COMPOSE_DIR="${REPO_ROOT}/test/docker-kerberos"
WORK_DIR="$(mktemp -d)"

for bin in "${UNITTEST}" "${DUCKDB}"; do
    if [ ! -x "${bin}" ]; then
        echo "ERROR: ${bin} not found. Run 'make' first." >&2
        exit 1
    fi
done

cleanup() {
    (cd "${COMPOSE_DIR}" && docker compose down -v >/dev/null 2>&1) || true
    rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

echo "==> Starting the KDC and the Kerberos-secured HDFS..."
cd "${COMPOSE_DIR}"
docker compose down -v >/dev/null 2>&1 || true
docker compose up -d --build --force-recreate

deadline=$(( $(date +%s) + 300 ))
until docker exec duckdb-hdfs-kdc test -f /keytabs/ready 2>/dev/null; do
    [ "$(date +%s)" -lt "${deadline}" ] || { echo "ERROR: the KDC did not start" >&2; docker compose logs --tail=50 >&2; exit 1; }
    sleep 2
done

echo "==> Logging in as hadoop@EXAMPLE.TEST..."
docker exec duckdb-hdfs-kdc bash -c 'echo hadooppw | kinit -c /tmp/client.ccache hadoop@EXAMPLE.TEST >/dev/null'
docker cp duckdb-hdfs-kdc:/tmp/client.ccache "${WORK_DIR}/krb5cc" >/dev/null

export KRB5_CONFIG="${REPO_ROOT}/test/hdfs-conf-kerberos/krb5.conf"
export HADOOP_CONF_DIR="${REPO_ROOT}/test/hdfs-conf-kerberos"
unset HADOOP_USER_NAME HADOOP_PROXY_USER

echo "==> Waiting for HDFS (a live DataNode, out of safe mode)..."
until KRB5CCNAME="FILE:${WORK_DIR}/krb5cc" "${DUCKDB}" -c \
    "COPY (SELECT 1 AS x) TO 'hdfs://localhost:19000/ready/probe.csv'" >/dev/null 2>&1; do
    if [ "$(date +%s)" -ge "${deadline}" ]; then
        echo "ERROR: HDFS did not become ready in time. Last error and logs:" >&2
        KRB5CCNAME="FILE:${WORK_DIR}/krb5cc" "${DUCKDB}" -c \
            "COPY (SELECT 1 AS x) TO 'hdfs://localhost:19000/ready/probe.csv'" >&2 || true
        docker compose logs --tail=80 hdfs >&2 || true
        exit 1
    fi
    sleep 3
done

cd "${REPO_ROOT}"
echo "==> Running Kerberos integration tests..."
HDFS_KERBEROS_TEST_RUNNING=1 KRB5CCNAME="FILE:${WORK_DIR}/krb5cc" \
    "${UNITTEST}" test/sql/hdfs_kerberos.test

echo "==> Running Kerberos tests without a ticket..."
HDFS_KERBEROS_NO_TICKET=1 KRB5CCNAME="FILE:${WORK_DIR}/no-such-ccache" \
    "${UNITTEST}" test/sql/hdfs_kerberos_no_ticket.test
