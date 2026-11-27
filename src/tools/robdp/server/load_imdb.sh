#!/usr/bin/env bash
# Load the JOB IMDB CSVs (the original imdb.tgz) into a database, checking
# every table's row count, then index and VACUUM ANALYZE it once.
#
#   load_imdb.sh CSV_DIR SCHEMA_SQL FKINDEXES_SQL
#
# Environment: PSQL (psql binary), PGHOST (default /tmp), PGPORT, PGUSER, DB
#   (default imdb), ENCODING (of the CSV files, default UTF8), DRY=1 (only
#   print what would be loaded).
#
# Headers: a file whose first line does not start with the integer id of a
# row has a header line, which COPY then skips (HEADER true); a file without
# one is loaded whole.  Using HEADER on a file without a header silently drops
# its first row, so the decision is printed per table and every count is
# compared with the JOB row counts below.
set -euo pipefail
csv=${1:?usage: $0 CSV_DIR SCHEMA_SQL FKINDEXES_SQL}
schema=${2:?schema.sql}
fk=${3:?fkindexes.sql}
PSQL=${PSQL:-psql}
DB=${DB:-imdb}
ENCODING=${ENCODING:-UTF8}
export PGHOST=${PGHOST:-/tmp}
q() { "$PSQL" -X -q -v ON_ERROR_STOP=1 -d "$DB" "$@"; }

# table:rows of the JOB database (WANT overrides it, for tests)
want=${WANT:-"aka_name:901343 aka_title:361472 cast_info:36244344 char_name:3140339
comp_cast_type:4 company_name:234997 company_type:4 complete_cast:135086
info_type:113 keyword:134170 kind_type:7 link_type:18
movie_companies:2609129 movie_info:14835720 movie_info_idx:1380035
movie_keyword:4523930 movie_link:29997 name:4167491 person_info:2963664
role_type:12 title:2528312"}

for tw in $want; do
  t=${tw%%:*}
  [ -f "$csv/$t.csv" ] || { echo "missing $csv/$t.csv" >&2; exit 1; }
done
if [ "${DRY:-0}" != 1 ]; then
  q -f "$schema"
fi
bad=0
for tw in $want; do
  t=${tw%%:*} rows=${tw##*:}
  first=$(head -c 200 "$csv/$t.csv" | head -n 1)
  if [[ $first =~ ^[0-9]+, ]]; then header=false; else header=true; fi
  echo "$t: header=$header, first line: ${first:0:60}"
  [ "${DRY:-0}" = 1 ] && continue
  q -c "\\copy $t FROM '$csv/$t.csv' WITH (FORMAT csv, ESCAPE '\\', HEADER $header, ENCODING '$ENCODING')"
  n=$(q -At -c "SELECT count(*) FROM $t")
  if [ "$n" != "$rows" ]; then
    echo "  ROW COUNT $n, JOB has $rows" >&2; bad=1
  else
    echo "  $n rows, as JOB"
  fi
done
[ "${DRY:-0}" = 1 ] && exit 0
[ $bad = 0 ] || { echo "row counts differ: not indexing" >&2; exit 1; }
q -f "$fk"
q -c "VACUUM (ANALYZE)"
q -At -c "SELECT 'pg_stats md5 ' || md5(string_agg(tablename || '.' || attname || ':' || null_frac || ':' || n_distinct || ':' ||
           coalesce(most_common_vals::text, '') || coalesce(most_common_freqs::text, '') || coalesce(histogram_bounds::text, ''),
           '|' order by tablename, attname)) FROM pg_stats WHERE schemaname = 'public'"
q -At -c "SELECT pg_size_pretty(pg_database_size('$DB'))"
