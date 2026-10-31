#!/usr/bin/env bash
# Runs cmdatl's real-atlc acceptance cases and fails unless every one of them ran and passed.
# ctest's test-acceptance entry is one process: when only some cases skip (a missing variable), Catch2 exits 0 and ctest reports it passed.
# So CI reads each case from a JUnit report instead.
# Usage: acceptance.sh TEST_CLI REPORT.xml
set -euo pipefail
bin="$1"
report="$2"

for name in CMDATL_TEST_ATLC CMDATL_TEST_PREFIX CMDATL_TEST_ATLC_EXAMPLES; do
  value="${!name:-}"
  if [ -z "$value" ]; then
    echo "::error::$name is not set"
    exit 1
  fi
  if [ ! -e "$value" ]; then
    echo "::error::$name names nothing: $value"
    exit 1
  fi
done

"$bin" "[acceptance]" --list-tests --verbosity quiet > "$report.cases"
if [ ! -s "$report.cases" ]; then
  echo "::error::test-cli holds no acceptance case"
  exit 1
fi

status=0
"$bin" "[acceptance]" --reporter "JUnit::out=$report" --reporter console::out=- || status=$?
if [ "$status" -ne 0 ]; then
  echo "::error::the acceptance cases failed or all skipped (exit $status)"
  exit 1
fi
if grep -q '<skipped' "$report"; then
  echo "::error::an acceptance case skipped:"
  grep -B1 '<skipped' "$report" | grep -o 'name="[^"]*"' || true
  exit 1
fi
# Every listed case must appear as a testcase of the report, compared by its decoded name.
python3 - "$report" "$report.cases" <<'PY'
import sys
import xml.etree.ElementTree as ET

report, listed = sys.argv[1], sys.argv[2]
ran = {case.get("name") for case in ET.parse(report).iter("testcase")}
skipped = [case.get("name") for case in ET.parse(report).iter("testcase") if case.find("skipped") is not None]
if skipped:
    print("::error::an acceptance case skipped: " + skipped[0])
    sys.exit(1)
with open(listed, encoding="utf-8") as cases:
    for name in (line.rstrip("\n") for line in cases):
        if name not in ran:
            print("::error::the acceptance case did not run: " + name)
            sys.exit(1)
PY
echo "acceptance: $(wc -l < "$report.cases" | tr -d ' ') cases ran"
