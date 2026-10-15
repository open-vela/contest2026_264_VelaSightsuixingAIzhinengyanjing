#!/usr/bin/env bash
#
# perf_test.sh -- run social sessions through the running UI and summarise
# their vs_perf totals, for unattended A/B comparisons.
#
#   ./perf_test.sh [-n runs] [-s seconds] [-t tag] [-p port]
#
# Each run is "velasight social run <seconds>" over serial_cmd.sh, so it goes
# through the same keys-to-pages path a user does.  The full capture of every
# run is kept in logs/runtime/perf-<tag>-<stamp>-<i>.log; stdout gets one
# summary line per run and then the mean of each field over the runs that
# completed.  Exit status is the number of runs that did not end in
# "vs_cli: ok".
#
#   -T "interval=500 fps=3 backlog=0"   pacing for these runs, applied over
#                                       the Kconfig defaults (social tune)
#   -D 23:00                            start no run that could end later

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
RUNS=3
SECONDS_PER_RUN=60
TAG=run
PORT=/dev/ttyUSB0
GAP=8

TUNE=""
DEADLINE=""

while getopts "n:s:t:p:T:D:h" opt; do
  case "$opt" in
    n) RUNS=$OPTARG ;;
    s) SECONDS_PER_RUN=$OPTARG ;;
    t) TAG=$OPTARG ;;
    p) PORT=$OPTARG ;;
    T) TUNE=$OPTARG ;;
    D) DEADLINE=$OPTARG ;;
    *) sed -n '3,19p' "$0"; exit 2 ;;
  esac
done

# -D HH:MM: start no run that could still be going at that time today.  A
# run is its sampling time plus a generous allowance for the minutes.
deadline_ok()
{
  local now end
  [ -z "$DEADLINE" ] && return 0
  now=$(date +%s)
  end=$(date -d "$DEADLINE" +%s) || return 1
  [ $((now + SECONDS_PER_RUN + 150)) -le "$end" ]
}

LOG_DIR="$SCRIPT_DIR/logs/runtime"
STAMP=$(date +%Y%m%d-%H%M%S)
mkdir -p "$LOG_DIR"
cd "$SCRIPT_DIR" || exit 2

# serial_cmd.sh, with the console already on the AP side.
console()
{
  local wait=$1 expect=$2 out=$3
  shift 3
  timeout $((wait + 60)) ./serial_cmd.sh -p "$PORT" -w "$wait" -e "$expect" \
    -o "$out" "$@" >/dev/null 2>&1
}

# After a flash or a reset the console is on the CP shell, which answers
# "cmd NOT found"; bridge once in that case only, because bridging from the
# AP side costs the full 40-attempt loop.
probe=$(mktemp)
console 15 'vs_cli: (ok|fail) status|cmd NOT found' "$probe" 'velasight status'
if grep -aq 'cmd NOT found' "$probe"; then
  timeout 120 ./serial_cmd.sh -p "$PORT" -b -w 20 \
    -e 'vs_cli: (ok|fail) status' -o "$probe" 'velasight status' \
    >/dev/null 2>&1
fi
if ! grep -aq 'vs_cli: ok status' "$probe"; then
  echo "perf_test: the UI did not answer 'velasight status'" >&2
  tr '\r' '\n' < "$probe" | tail -5 >&2
  rm -f "$probe"
  exit 2
fi
rm -f "$probe"

# Every invocation starts from the Kconfig pacing, so a tuning left by an
# earlier run cannot leak into this one; -T then applies on top of it.
tunelog=$(mktemp)
# shellcheck disable=SC2086
console 15 'vs_cli: (ok|fail) social tune' "$tunelog" \
  "velasight social tune default $TUNE"
if ! grep -aq 'vs_cli: ok social tune' "$tunelog"; then
  echo "perf_test: tuning \"$TUNE\" was not accepted" >&2
  tr '\r' '\n' < "$tunelog" | grep -a 'vs_cli' >&2
  rm -f "$tunelog"
  exit 2
fi
tr '\r' '\n' < "$tunelog" | grep -a -o 'interval=.*' | head -1 | \
  sed "s/^/tune tag=$TAG /"
rm -f "$tunelog"

# Until the previous session has fully unwound (status says social=0), so
# one that is still draining a stuck upload does not turn the next run into
# a refused start.  Bounded by the transport's own 120 s receive timeout.
wait_idle()
{
  local idle tries=0
  idle=$(mktemp)
  while [ "$tries" -lt 20 ]; do
    console 10 'vs_cli: (ok|fail) status' "$idle" 'velasight status'
    if grep -aq 'vs_cli: status .* social=0' "$idle"; then
      rm -f "$idle"
      return 0
    fi
    tries=$((tries + 1))
    sleep 8
  done
  rm -f "$idle"
  echo "perf_test: a session was still active after the wait" >&2
  return 1
}

failed=0
logs=()
for i in $(seq 1 "$RUNS"); do
  log="$LOG_DIR/perf-$TAG-$STAMP-$i.log"
  if ! deadline_ok; then
    echo "perf_test: deadline $DEADLINE, not starting run $i"
    break
  fi
  wait_idle
  console $((SECONDS_PER_RUN + 480)) 'vs_cli: (ok|fail) social' "$log" \
    "velasight social run $SECONDS_PER_RUN"
  logs+=("$log")
  grep -aq 'vs_cli: ok social' "$log" || failed=$((failed + 1))
  [ "$i" -lt "$RUNS" ] && sleep "$GAP"
done

python3 - "$TAG" "${logs[@]}" <<'EOF'
import re
import sys

tag, paths = sys.argv[1], sys.argv[2:]

def pairs(line):
    out = {}
    for key, value in re.findall(r'(\w+)=("[^"]*"|\S+)', line):
        out[key] = value.strip('"')
    return out

def nums(value):
    try:
        return [float(x) for x in value.split('/')]
    except ValueError:
        return None

SHOW = ['up', 'kbps', 'qw', 'reg', 'put', 'aput', 'lat', 'cld', 'res',
        'rep', 'poll', 'ids', 'pms', 'yld', 'tcp', 'drop', 'iob']
rows = []

for n, path in enumerate(paths, 1):
    text = open(path, 'rb').read().decode('utf-8', 'replace')
    text = re.sub(r'\x1b\[[0-9;]*m', '', text).replace('\r', '\n')
    total = [l for l in text.split('\n') if 'vs_perf: total' in l]
    ok = re.search(r'vs_cli: ok social', text) is not None
    why = re.search(r'vs_cli: fail social\S* .*?why="([^"]*)"', text)
    sid = re.search(r'vs_perf: cfg sid=(\S+)', text)
    stages = dict((m.group(1), int(m.group(2))) for m in re.finditer(
        r'finalize: (\S+(?: \S+)*?) took (\d+) ms', text))
    refusals = sorted(set(re.findall(r'image refused: (.*)', text)))
    row = {'ok': ok}
    line = 'run=%d ok=%d sid=%s' % (n, ok, sid.group(1) if sid else '-')
    if total:
        kv = pairs(total[-1].split('vs_perf: total', 1)[1])
        for key in SHOW:
            if key in kv:
                line += ' %s=%s' % (key, kv[key])
                row[key] = nums(kv[key])
        tcp = row.get('tcp')
        if tcp and tcp[0] > 0:
            row['rexmit%'] = [100.0 * tcp[1] / tcp[0]]
            line += ' rexmit%%=%.1f' % row['rexmit%'][0]
    # Whether the cloud kept up: answers per uploaded image, how far its
    # latency drifted from the first third of the windows to the last, and
    # the most results ever awaited at once.
    wins = []
    for l in text.split('\n'):
        if 'vs_perf: t=' in l:
            wins.append(pairs(l.split('vs_perf:', 1)[1]))
    if total and row.get('up') and row.get('res') and row['up'][0] > 0:
        row['ans%'] = [100.0 * (row['res'][0] + row['res'][1]) /
                       row['up'][0]]
        line += ' ans%%=%.0f' % row['ans%'][0]
    clds = [nums(w['cld'])[0] for w in wins
            if 'cld' in w and nums(w['cld']) and nums(w['cld'])[0] > 0]
    if len(clds) >= 3:
        third = max(1, len(clds) // 3)
        drift = sum(clds[-third:]) / third - sum(clds[:third]) / third
        row['drift'] = [drift]
        line += ' drift=%+d' % drift
    outs = [int(w['out']) for w in wins if w.get('out', '').isdigit()]
    if outs:
        row['outmax'] = [float(max(outs))]
        line += ' outmax=%d' % max(outs)
    for name, short in (('waiting for the minutes', 'minutes_ms'),
                        ('stopping capture', 'stop_ms'),
                        ('fetching the spoken minutes', 'fetch_ms')):
        if name in stages:
            row[short] = [float(stages[name])]
            line += ' %s=%d' % (short, stages[name])
    if why:
        line += ' why="%s"' % why.group(1)
    if refusals:
        line += ' refused="%s"' % '|'.join(refusals)[:120]
    print(line)
    rows.append(row)

done = [r for r in rows if r['ok']]
if done:
    keys = [k for k in SHOW + ['rexmit%', 'ans%', 'drift', 'outmax',
                               'minutes_ms', 'stop_ms', 'fetch_ms']
            if any(k in r for r in done)]
    line = 'mean tag=%s runs=%d/%d' % (tag, len(done), len(rows))
    for key in keys:
        have = [r[key] for r in done if key in r and r[key]]
        if not have:
            continue
        width = max(len(v) for v in have)
        cols = []
        for i in range(width):
            vals = [v[i] for v in have if len(v) > i]
            cols.append(sum(vals) / len(vals))
        line += ' %s=%s' % (key, '/'.join(
            ('%.1f' % c) if key in ('kbps', 'rexmit%', 'ids')
            else '%d' % round(c)
            for c in cols))
    print(line)
EOF

exit "$failed"
