#!/usr/bin/env bash
# One-shot: start a KDC and a Kerberos-secured HDFS (test/docker-kerberos), log
# in as hadoop@EXAMPLE.TEST, and run the Kerberos SQL tests once per wire
# protection mode, then tear down.
#
# Usage: test/scripts/run_kerberos_tests.sh [mode...]   (needs Docker and a prior `make`)
#
# Modes (default: all): none, integrity, privacy, aes, encrypt-key; see
# test/docker-kerberos/modes/.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UNITTEST="${REPO_ROOT}/build/release/test/unittest"
DUCKDB="${REPO_ROOT}/build/release/duckdb"
COMPOSE_DIR="${REPO_ROOT}/test/docker-kerberos"
WORK_DIR="$(mktemp -d)"
MODES=("$@")
if [ ${#MODES[@]} -eq 0 ]; then
    MODES=(none integrity privacy aes encrypt-key)
fi

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

# A client config dir for the given mode: the base Kerberos client config plus
# the mode's client-side settings (name=value pairs).
client_conf() {
    local dir="${WORK_DIR}/conf-$1"
    shift
    rm -rf "${dir}" && mkdir -p "${dir}"
    cp "${REPO_ROOT}"/test/hdfs-conf-kerberos/*.xml "${dir}/"
    python3 -I - "${dir}/hdfs-site.xml" "$@" <<'PY'
import sys
path, pairs = sys.argv[1], sys.argv[2:]
props = "".join(
    "    <property><name>%s</name><value>%s</value></property>\n" % tuple(p.split("=", 1)) for p in pairs)
text = open(path).read().replace("</configuration>", props + "</configuration>")
open(path, "w").write(text)
PY
    echo "${dir}"
}

client_settings() {
    case "$1" in
        none) ;;
        integrity) echo "dfs.data.transfer.protection=integrity" ;;
        privacy | aes) echo "dfs.data.transfer.protection=privacy" ;;
        encrypt-key) echo "dfs.encrypt.data.transfer=true" ;;
        *) echo "ERROR: unknown mode $1" >&2; exit 1 ;;
    esac
}

probe() { # conf_dir -> succeeds if a write works
    KRB5CCNAME="FILE:${WORK_DIR}/krb5cc" HADOOP_CONF_DIR="$1" "${DUCKDB}" -c \
        "COPY (SELECT 1 AS x) TO 'hdfs://localhost:19000/ready/probe.csv'" >/dev/null 2>&1
}

expect_failure() { # description conf_dir
    if probe "$2"; then
        echo "ERROR: expected failure: $1" >&2
        exit 1
    fi
    echo "OK (refused): $1"
}

echo "==> Starting the KDC..."
cd "${COMPOSE_DIR}"
docker compose down -v >/dev/null 2>&1 || true
docker compose build -q
docker compose up -d kdc
deadline=$(( $(date +%s) + 300 ))
until docker exec duckdb-hdfs-kdc test -f /keytabs/ready 2>/dev/null; do
    [ "$(date +%s)" -lt "${deadline}" ] || { echo "ERROR: the KDC did not start" >&2; docker compose logs --tail=50 >&2; exit 1; }
    sleep 2
done

echo "==> Logging in as hadoop@EXAMPLE.TEST..."
docker exec duckdb-hdfs-kdc bash -c 'echo hadooppw | kinit -c /tmp/client.ccache hadoop@EXAMPLE.TEST >/dev/null'
docker cp duckdb-hdfs-kdc:/tmp/client.ccache "${WORK_DIR}/krb5cc" >/dev/null

export KRB5_CONFIG="${REPO_ROOT}/test/hdfs-conf-kerberos/krb5.conf"
unset HADOOP_USER_NAME HADOOP_PROXY_USER

for mode in "${MODES[@]}"; do
    echo "==> [${mode}] Starting the Kerberos-secured HDFS..."
    cd "${COMPOSE_DIR}"
    HDFS_WIRE_MODE="${mode}" docker compose up -d --force-recreate hdfs >/dev/null
    # shellcheck disable=SC2046
    CONF="$(client_conf "${mode}" $(client_settings "${mode}"))"

    echo "==> [${mode}] Waiting for HDFS (a live DataNode, out of safe mode)..."
    deadline=$(( $(date +%s) + 300 ))
    until probe "${CONF}"; do
        if [ "$(date +%s)" -ge "${deadline}" ]; then
            echo "ERROR: HDFS did not become ready in time. Last error and logs:" >&2
            KRB5CCNAME="FILE:${WORK_DIR}/krb5cc" HADOOP_CONF_DIR="${CONF}" "${DUCKDB}" -c \
                "COPY (SELECT 1 AS x) TO 'hdfs://localhost:19000/ready/probe.csv'" >&2 || true
            docker compose logs --tail=80 hdfs >&2 || true
            exit 1
        fi
        sleep 3
    done

    cd "${REPO_ROOT}"
    echo "==> [${mode}] Running Kerberos integration tests..."
    HDFS_KERBEROS_TEST_RUNNING=1 KRB5CCNAME="FILE:${WORK_DIR}/krb5cc" HADOOP_CONF_DIR="${CONF}" \
        "${UNITTEST}" test/sql/hdfs_kerberos.test

    # The protection is enforced, not just negotiated.
    case "${mode}" in
        integrity | privacy | aes)
            expect_failure "a client without dfs.data.transfer.protection" "$(client_conf "${mode}-noprot")"
            ;;
    esac
    if [ "${mode}" = encrypt-key ]; then
        expect_failure "a client without dfs.encrypt.data.transfer" "$(client_conf "${mode}-nokey")"
    fi
    case "${mode}" in
        privacy | aes)
            expect_failure "a client accepting only authentication-level RPC" \
                "$(client_conf "${mode}-rpcauth" "hadoop.rpc.protection=authentication" $(client_settings "${mode}"))"
            ;;
    esac
done

echo "==> Running Kerberos tests without a ticket..."
HDFS_KERBEROS_NO_TICKET=1 KRB5CCNAME="FILE:${WORK_DIR}/no-such-ccache" \
    HADOOP_CONF_DIR="${REPO_ROOT}/test/hdfs-conf-kerberos" \
    "${UNITTEST}" test/sql/hdfs_kerberos_no_ticket.test
