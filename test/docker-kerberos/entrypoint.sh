#!/usr/bin/env bash
# Wait for the KDC's keytab, then format (once) and run a Kerberos-secured
# NameNode + DataNode.
set -euo pipefail
export HADOOP_CONF_DIR=/etc/hadoop-conf
until [ -f /keytabs/ready ]; do sleep 1; done

# Apply the wire-protection mode's settings to hdfs-site.xml.
MODE="${HDFS_WIRE_MODE:-none}"
MODE_FILE="/etc/hadoop-conf/modes/${MODE}.xml"
[ -f "${MODE_FILE}" ] || { echo "Unknown HDFS_WIRE_MODE ${MODE}" >&2; exit 1; }
cp /etc/hadoop-conf/hdfs-site.xml /tmp/hdfs-site.base.xml
sed -e "/<\/configuration>/{r ${MODE_FILE}" -e "a </configuration>" -e "d}" /tmp/hdfs-site.base.xml > /etc/hadoop-conf/hdfs-site.xml
echo "HDFS wire mode: ${MODE}"

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
