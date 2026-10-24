#!/usr/bin/env bash
# Benchmark the hdfs extension against the Docker test cluster.
#
# Usage: test/bench/run_bench.sh [--latency MS] [bench.py options...]
#   --latency MS   route NameNode/DataNode traffic through toxiproxy, adding MS
#                  milliseconds each way (models a real network; default: none)
#   bench.py options: --runs N, --label TEXT, --save NAME, --compare NAME, --only TEXT
#
# Environment: BENCH_REUSE=1 reuses a running cluster (and its seeded data);
# BENCH_KEEP=1 leaves the cluster running afterwards; BENCH_QUICK=1 seeds a
# much smaller data set; BENCH_HADOOP_CONF_DIR replaces the client config
# (test/hdfs-conf). Needs Docker and a prior `make`.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DUCKDB="${REPO_ROOT}/build/release/duckdb"
LATENCY_MS=0
if [ "${1:-}" = "--latency" ]; then
    LATENCY_MS="$2"
    shift 2
fi

[ -x "${DUCKDB}" ] || { echo "ERROR: ${DUCKDB} not found. Run 'make' first." >&2; exit 1; }

export HADOOP_CONF_DIR="${BENCH_HADOOP_CONF_DIR:-${REPO_ROOT}/test/hdfs-conf}"
export HADOOP_USER_NAME=hadoop
if [ "${LATENCY_MS}" != 0 ]; then
    export HDFS_COMPOSE_ARGS="-f docker-compose.yml -f ${REPO_ROOT}/test/bench/docker-compose.latency.yml"
fi

TOXI="http://localhost:8474"
toxi() { curl -fsS -X "$1" -H 'Content-Type: application/json' ${3:+-d "$3"} "${TOXI}$2" >/dev/null; }

cleanup() {
    if [ "${LATENCY_MS}" != 0 ]; then
        for p in nn dn; do toxi DELETE "/proxies/${p}/toxics/latency_down" 2>/dev/null || true; done
    fi
    if [ "${BENCH_KEEP:-0}" != 1 ]; then
        "${REPO_ROOT}/test/scripts/hdfs_down.sh" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

if [ "${BENCH_REUSE:-0}" != 1 ]; then
    "${REPO_ROOT}/test/scripts/hdfs_up.sh" >/dev/null
fi

if [ "${LATENCY_MS}" != 0 ]; then
    echo "==> Proxying NameNode and DataNode traffic through toxiproxy..."
    for i in $(seq 1 30); do curl -fsS "${TOXI}/version" >/dev/null 2>&1 && break; sleep 1; done
    for p in nn:9000 dn:9866; do
        name="${p%%:*}" port="${p##*:}"
        toxi DELETE "/proxies/${name}" 2>/dev/null || true
        toxi POST /proxies "{\"name\":\"${name}\",\"listen\":\"0.0.0.0:${port}\",\"upstream\":\"hdfs:${port}\",\"enabled\":true}"
    done
fi

if ! "${DUCKDB}" -no-agent -csv -noheader -c "SELECT hdfs_exists('hdfs://localhost:9000/bench/_seeded')" | grep -q true; then
    echo "==> Seeding benchmark data (once per cluster)..."
    if [ "${BENCH_QUICK:-0}" = 1 ]; then
        SMALL=20000 BIG=1000000 CSV=500000 TREE=100000
    else
        SMALL=200000 BIG=20000000 CSV=8000000 TREE=1000000
    fi
    "${DUCKDB}" -no-agent -c "
        COPY (SELECT range % 2000 AS p, range AS v FROM range(${SMALL}))
            TO 'hdfs://localhost:9000/bench/small' (FORMAT parquet, PARTITION_BY (p));
        COPY (SELECT range AS v, md5(range::VARCHAR) AS s FROM range(${BIG}))
            TO 'hdfs://localhost:9000/bench/big.parquet' (FORMAT parquet);
        COPY (SELECT range AS v, md5(range::VARCHAR) AS s FROM range(${CSV}))
            TO 'hdfs://localhost:9000/bench/big.csv' (FORMAT csv, HEADER);
        COPY (SELECT 2020 + range % 5 AS y, range % 12 AS m, range % 28 AS d, range AS v FROM range(${TREE}))
            TO 'hdfs://localhost:9000/bench/tree' (FORMAT parquet, PARTITION_BY (y, m, d));
        COPY (SELECT 1 AS done) TO 'hdfs://localhost:9000/bench/_seeded' (FORMAT csv);"
fi

if [ "${LATENCY_MS}" != 0 ]; then
    for p in nn dn; do
        toxi POST "/proxies/${p}/toxics" \
            "{\"name\":\"latency_down\",\"type\":\"latency\",\"stream\":\"downstream\",\"attributes\":{\"latency\":$((LATENCY_MS * 2))}}"
    done
    echo "==> Added ${LATENCY_MS} ms each way ($((LATENCY_MS * 2)) ms per round trip)"
fi

echo "==> Running benchmarks..."
python3 -I "${REPO_ROOT}/test/bench/bench.py" --duckdb "${DUCKDB}" "$@"
