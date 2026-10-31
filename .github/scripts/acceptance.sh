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
while IFS= read -r case_name; do
  escaped="$(printf '%s' "$case_name" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g' -e 's/"/\&quot;/g')"
  if ! grep -qF "name=\"$escaped\"" "$report"; then
    echo "::error::the acceptance case did not run: $case_name"
    exit 1
  fi
done < "$report.cases"
echo "acceptance: $(wc -l < "$report.cases" | tr -d ' ') cases ran"
