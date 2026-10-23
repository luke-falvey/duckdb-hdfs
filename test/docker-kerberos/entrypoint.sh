#!/usr/bin/env bash
# Wait for the KDC's keytab, then format (once) and run a Kerberos-secured
# NameNode + DataNode.
set -euo pipefail
export HADOOP_CONF_DIR=/etc/hadoop-conf
until [ -f /keytabs/ready ]; do sleep 1; done

mkdir -p /tmp/hdfs/name /tmp/hdfs/data
if [ ! -d /tmp/hdfs/name/current ]; then
    hdfs namenode -format -force -nonInteractive
fi
hdfs namenode &
NN_PID=$!
hdfs datanode &
DN_PID=$!
trap 'kill "${NN_PID}" "${DN_PID}" 2>/dev/null || true' TERM INT
while kill -0 "${NN_PID}" 2>/dev/null && kill -0 "${DN_PID}" 2>/dev/null; do
    sleep 5
done
echo "A HDFS daemon exited; shutting down."
