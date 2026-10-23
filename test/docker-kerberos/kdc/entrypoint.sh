#!/usr/bin/env bash
# Create the test realm, the HDFS service principals and their keytab, and a
# user principal for the tests, then run the KDC in the foreground.
set -euo pipefail
REALM=EXAMPLE.TEST
mkdir -p /keytabs
if [ ! -f /var/lib/krb5kdc/principal ]; then
    kdb5_util create -s -r "${REALM}" -P masterpw >/dev/null
fi
for p in nn/localhost dn/localhost HTTP/localhost; do
    kadmin.local -q "addprinc -randkey ${p}@${REALM}" >/dev/null
done
rm -f /keytabs/hdfs.keytab
kadmin.local -q "ktadd -k /keytabs/hdfs.keytab nn/localhost@${REALM} dn/localhost@${REALM} HTTP/localhost@${REALM}" >/dev/null
# The test client logs in with a password (see run_kerberos_tests.sh).
kadmin.local -q "addprinc -pw hadooppw hadoop@${REALM}" >/dev/null
chmod 644 /keytabs/hdfs.keytab
touch /keytabs/ready
echo "KDC ready"
exec krb5kdc -n
