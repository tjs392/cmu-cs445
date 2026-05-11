#!/usr/bin/env bash
# Sanity-checks HW1 queries against the example first rows from the spec.
# Not a full grader — only verifies first-row format/correctness.

set -u
DB="lahman-cmudb2025.db"
DIR="placeholder"

# expected first data row for each question (Q1 has full output, others first row only)
declare -A EXPECTED=(
  [q1_sample]="AL Central"
  [q2_hr]="Bobby (Robert Leigh) Higginson|30"
  [q3_mvp]="Ichiro|SEA|10"
  [q5_hof]="Addie (Adrian) Joss|Bill (William Henry) Bernhard|1902"
)

run_query() {
  # -list: pipe-separated; -noheader: drop the column-name row
  duckdb -list -noheader "$DB" < "$DIR/$1.duckdb.sql" 2>&1
}

check() {
  local q="$1" expected="$2"
  if [[ ! -s "$DIR/$q.duckdb.sql" ]]; then
    printf "  %-12s SKIP (empty file)\n" "$q"; return
  fi
  local out first
  out="$(run_query "$q")"
  first="$(echo "$out" | head -n1)"
  if [[ "$first" == "$expected" ]]; then
    printf "  %-12s PASS  (first row matches)\n" "$q"
  else
    printf "  %-12s FAIL\n    expected: %s\n    got:      %s\n" "$q" "$expected" "$first"
  fi
}

echo "== first-row checks =="
for q in q1_sample q2_hr q3_mvp q5_hof; do
  check "$q" "${EXPECTED[$q]}"
done

# Q4: spec only gives one example row that should appear *somewhere*, not necessarily first
echo "== q4_award contains-row check =="
if [[ -s "$DIR/q4_award.duckdb.sql" ]]; then
  if run_query q4_award | grep -qxF "National League|Atlanta Braves|3"; then
    echo "  q4_award     PASS  (contains 'National League|Atlanta Braves|3')"
  else
    echo "  q4_award     FAIL  (example row not found in output)"
  fi
else
  echo "  q4_award     SKIP (empty file)"
fi

# Q6: upsert. Run the user's statement, then run the spec's verification SELECT and look for the two known rows.
echo "== q6_upsert post-state check =="
if [[ -s "$DIR/q6_upsert.duckdb.sql" ]]; then
  TMP="$(mktemp)"
  cp "$DB" "$TMP"   # work on a copy so we don't corrupt the real DB
  duckdb "$TMP" < "$DIR/q6_upsert.duckdb.sql" >/dev/null 2>&1
  POST="$(duckdb -list -noheader "$TMP" -c "SELECT yearID, lgID, teamID, franchID, teamRank, attendance FROM teams WHERE yearID = 2024 ORDER BY lgID, teamID, franchID, teamRank, attendance;")"
  rm -f "$TMP"
  ok=1
  for row in "2024|AL|HOU|HOU|1|-1" "2024|AL|KCA|KCR|2|1658347"; do
    if ! echo "$POST" | grep -qxF "$row"; then
      echo "  q6_upsert    FAIL  (missing row: $row)"; ok=0
    fi
  done
  (( ok )) && echo "  q6_upsert    PASS  (both expected rows present)"
else
  echo "  q6_upsert    SKIP (empty file)"
fi