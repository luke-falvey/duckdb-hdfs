#!/usr/bin/env bash
# Format (once) and run a single-node HDFS: NameNode + DataNode in one container.
set -euo pipefail

export HADOOP_CONF_DIR=/etc/hadoop-conf

# HDFS_SITE_EXTRA: extra hdfs-site.xml properties, "name=value,name=value"
# (run_hdfs_tests.sh uses this for small listing limits, to exercise paging).
if [ -n "${HDFS_SITE_EXTRA:-}" ]; then
    props=""
    IFS=',' read -ra pairs <<< "${HDFS_SITE_EXTRA}"
    for pair in "${pairs[@]}"; do
        props+="  <property><name>${pair%%=*}</name><value>${pair#*=}</value></property>\n"
    done
    # The image's conf dir isn't writable by the hadoop user: use a copy.
    rm -rf /tmp/hadoop-conf && cp -R "${HADOOP_CONF_DIR}" /tmp/hadoop-conf
    export HADOOP_CONF_DIR=/tmp/hadoop-conf
    sed -i "s#</configuration>#${props}</configuration>#" "${HADOOP_CONF_DIR}/hdfs-site.xml"
fi

NAME_DIR=/tmp/hdfs/name
mkdir -p /tmp/hdfs/name /tmp/hdfs/data

if [ ! -d "${NAME_DIR}/current" ]; then
    echo "Formatting NameNode..."
    hdfs namenode -format -force -nonInteractive
fi

# The KMS serves encryption-zone keys; the NameNode asks it for new file keys.
echo "Starting KMS..."
mkdir -p /tmp/kms
hadoop kms &
KMS_PID=$!

echo "Starting NameNode..."
hdfs namenode &
NN_PID=$!

echo "Starting DataNode..."
hdfs datanode &
DN_PID=$!

# Forward termination to the daemons so `docker compose down` is clean.
trap 'kill "${NN_PID}" "${DN_PID}" "${KMS_PID}" 2>/dev/null || true' TERM INT

# Stay in the foreground until one of the daemons exits. (The image's bash
# predates `wait -n`, so poll liveness instead.)
while kill -0 "${NN_PID}" 2>/dev/null && kill -0 "${DN_PID}" 2>/dev/null; do
    sleep 5
done

echo "A HDFS daemon exited; shutting down."
