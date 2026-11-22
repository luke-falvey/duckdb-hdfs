# Writes future_provider.duckdb_secret: a persistent secret of TYPE hdfs with PROVIDER future, as a newer version of
# the extension might write it. A stock DuckDB (1.4, readable by 2.0) writes an `http` secret, and the two
# length-prefixed strings are patched in place to strings of the same length.
# Usage: python -m venv v && v/bin/pip install duckdb==1.4.1 && v/bin/python make_fixture.py <output dir>
import duckdb, sys, pathlib
d = pathlib.Path(sys.argv[1]).resolve()
con = duckdb.connect()
con.execute(f"SET secret_directory = '{d}'")
con.execute("CREATE PERSISTENT SECRET future_provider (TYPE http, PROVIDER config, SCOPE 'hdfs://127.0.0.1:1/future')")
con.close()
f = d / "future_provider.duckdb_secret"
b = f.read_bytes()
for old, new in [(b"\x04http", b"\x04hdfs"), (b"\x06config", b"\x06future")]:
    assert b.count(old) == 1, (old, b.count(old))
    b = b.replace(old, new)
f.write_bytes(b)
