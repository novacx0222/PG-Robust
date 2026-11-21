import sys, os, json, duckdb
src, dst, out = sys.argv[1], sys.argv[2], sys.argv[3]
con = duckdb.connect()
schema = {}
for f in sorted(os.listdir(src)):
    if not f.endswith('.parquet'):
        continue
    t = f[len('job_'):-len('.parquet')]
    p = os.path.join(src, f)
    cols = con.execute("DESCRIBE SELECT * FROM read_parquet(?)", [p]).fetchall()
    schema[t] = [(c[0], c[1]) for c in cols]
    n = con.execute("SELECT count(*) FROM read_parquet(?)", [p]).fetchone()[0]
    con.execute("COPY (SELECT * FROM read_parquet('%s')) TO '%s' (FORMAT CSV, HEADER false, NULL '')"
                % (p.replace("'", "''"), os.path.join(dst, t + '.csv').replace("'", "''")))
    print(t, n, schema[t])
json.dump(schema, open(out, 'w'), indent=1)
