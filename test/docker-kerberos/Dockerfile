# Kerberos-secured single-node HDFS for the Kerberos integration tests.
FROM apache/hadoop:3.4.1

USER root
ENV HADOOP_CONF_DIR=/etc/hadoop-conf
COPY core-site.xml hdfs-site.xml /etc/hadoop-conf/
COPY krb5.conf /etc/krb5.conf
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh
USER hadoop
ENTRYPOINT ["/entrypoint.sh"]
