#!/usr/bin/env bash
# Stop and remove the single-node HDFS test cluster.
#
# Usage: test/scripts/hdfs_down.sh
set -euo pipefail
# HDFS_COMPOSE_ARGS: extra `docker compose` arguments, e.g. "-f docker-compose.yml -f <override>"
# (the benchmark's latency mode uses this).

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${REPO_ROOT}/test/docker"

echo "==> Stopping HDFS container..."
docker compose ${HDFS_COMPOSE_ARGS:-} down -v
