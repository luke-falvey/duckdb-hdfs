# Kerberos-secured single-node HDFS for the Kerberos integration tests.
FROM apache/hadoop:3.4.1

USER root
ENV HADOOP_CONF_DIR=/etc/hadoop-conf
COPY core-site.xml hdfs-site.xml ssl-server.xml /etc/hadoop-conf/
COPY modes /etc/hadoop-conf/modes
RUN keytool -genkeypair -alias localhost -keyalg RSA -keysize 2048 -validity 3650 \
        -dname CN=localhost -keystore /etc/hadoop-conf/keystore.jks \
        -storepass changeit -keypass changeit -noprompt \
    && chmod 644 /etc/hadoop-conf/keystore.jks
COPY krb5.conf /etc/krb5.conf
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh \
    && chown -R hadoop /etc/hadoop-conf   # the entrypoint writes the mode's hdfs-site.xml
USER hadoop
ENTRYPOINT ["/entrypoint.sh"]
