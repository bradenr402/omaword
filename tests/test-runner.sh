#!/usr/bin/env bash
# Exercise orchestration without compiling Qt projects.
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
workspace=$(mktemp -d "${TMPDIR:-/tmp}/omaword-runner-test.XXXXXX")
running=
trap 'if [[ -n $running ]]; then kill "$running" 2>/dev/null || true; wait "$running" 2>/dev/null || true; fi; rm -rf -- "$workspace"' EXIT
export STATE="$workspace/state"
mkdir -p "$workspace/checkout/bin" "$workspace/checkout/tests" "$workspace/tools" "$STATE"
cp "$root/bin/test" "$workspace/checkout/bin/test"
for project in backend ui importer_versions blockannotations singleinstance windows; do
    touch "$workspace/checkout/tests/$project.pro"
done
cat > "$workspace/tools/qmake6" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
project=${1##*/}
project=${project%.pro}
printf '%s\n' "$project" > project
ln -sf "$TOOLS/worker" "tst_$project"
SH
cat > "$workspace/tools/worker" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
project=$(<project)
phase=test
weight=1
if [[ ${0##*/} == make ]]; then
    phase=build
    [[ $# == 1 && $1 == -j"$EXPECTED_JOBS" ]]
    weight=${1#-j}
fi
exec 9>"$STATE/lock"
flock 9
printf '%s\n' "$weight" > "$STATE/active-$BASHPID"
total=0
for entry in "$STATE"/active-*; do total=$((total + $(<"$entry"))); done
(( total <= JOBS )) || { printf 'CPU budget exceeded\n'; exit 91; }
[[ $total -le 1 ]] || touch "$STATE/overlap"
flock -u 9
trap 'rm -f "$STATE/active-$BASHPID"' EXIT
if [[ $phase == test ]]; then
    printf '%s\n' "$@" > "$STATE/$project.args"
    for variable in OMAWORD_TEST_KJV OMAWORD_TEST_KJV_EPUB OMAWORD_TEST_EPUB; do
        [[ ${!variable} == "$FIXTURE" ]]
    done
    [[ $QT_QPA_PLATFORM == offscreen && $QT_QUICK_BACKEND == software ]]
fi
if [[ ${HOLD:-0} == 1 ]]; then
    printf '%s\n' "$BASHPID" > "$STATE/held"
    sleep 30
else
    sleep 0.15
fi
printf '%s %s finished\n' "$project" "$phase"
touch "$STATE/$project.$phase"
if [[ ${FAIL_PHASE:-} == "$phase" && $project == backend ]]; then exit 7; fi
SH
chmod +x "$workspace/tools/qmake6" "$workspace/tools/worker"
ln -s worker "$workspace/tools/make"
export TOOLS="$workspace/tools" PATH="$workspace/tools:$PATH"
touch "$workspace/input book.epub"
export FIXTURE="$workspace/input book.epub"
export OMAWORD_TEST_KJV='input book.epub' OMAWORD_TEST_KJV_EPUB='input book.epub' OMAWORD_TEST_EPUB='input book.epub'
export BUILD_DIR='output tree/../output tree' JOBS=3 TEST_JOBS=2 EXPECTED_JOBS=1
cd "$workspace"
runner="$workspace/checkout/bin/test"
run_status() {
    status=0
    bash "$runner" "$@" > "$workspace/output" 2>&1 || status=$?
}
assert_output() { [[ $(<"$workspace/output") == *"$1"* ]]; }
assert_idle() {
    local active=("$STATE"/active-*)
    [[ ! -e ${active[0]} ]]
}
export FAIL_PHASE=test
run_status --suite backend --suite ui --suite annotations
[[ $status == 7 && -e $STATE/overlap && -e $STATE/ui.test && -e $STATE/blockannotations.test ]]
assert_output '[backend] FAIL (exit=7'
assert_output '  backend test finished'
assert_output '[ui] PASS'
assert_output '[annotations] PASS'
[[ -f $workspace/output\ tree/test-logs/backend.log ]]
assert_idle
printf 'PASS bounded parallel failure, readable logs, remaining suites waited\n'

rm -f "$STATE"/*.test "$STATE"/*.build
export FAIL_PHASE=build
run_status --suite backend --suite ui
[[ $status == 7 && ! -e $STATE/backend.test && -e $STATE/ui.test ]]
assert_idle
printf 'PASS build failure skips its test and preserves failure status\n'

unset FAIL_PHASE
export EXPECTED_JOBS=3
run_status --suite annotations selectedFunction -o 'result file.txt,txt'
[[ $status == 0 ]]
[[ $(<"$STATE/blockannotations.args") == $'selectedFunction\n-o\nresult file.txt,txt' ]]
assert_output '[annotations] PASS'
[[ $(<"$workspace/output") != *'[backend] START'* ]]
printf 'PASS targeted Qt arguments and caller-relative paths\n'

export JOBS=1 TEST_JOBS=8 EXPECTED_JOBS=1
run_status
[[ $status == 0 ]]
for project in backend ui importer_versions blockannotations singleinstance windows; do
    [[ -f $STATE/$project.test ]]
done
assert_output '[windows] PASS'
assert_idle
run_status --suite all selectedFunction
[[ $status == 1 ]]
assert_output 'require a single --suite target'
printf 'PASS default all discovers windows, clamps concurrency, rejects ambiguous Qt arguments\n'

HOLD=1 bash "$runner" --suite annotations > "$workspace/output" 2>&1 &
running=$!
for (( attempt=0; attempt < 100; attempt++ )); do
    [[ ! -f $STATE/held ]] || break
    sleep 0.05
done
[[ -f $STATE/held ]]
worker=$(<"$STATE/held")
kill -TERM "$running"
status=0
wait "$running" || status=$?
running=
[[ $status == 143 ]]
for (( attempt=0; attempt < 100; attempt++ )); do
    if ! kill -0 "$worker" 2>/dev/null; then break; fi
    sleep 0.05
done
! kill -0 "$worker" 2>/dev/null
assert_idle
printf 'PASS interruption terminates active worker process groups\n'
