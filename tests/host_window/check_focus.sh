#!/bin/bash
# check_focus.sh <program> [seconds] [args...] -- run a program that opens a
# background window and ask the window server, from outside, which
# application is in front: before, every half second while it runs, and
# after. Fails if the front application ever changes or is the program.
#   tests/host_window/check_focus.sh build/tests/host_window/host_window_focus 5
PROG=$1; SECS=${2:-5}; shift 2 2>/dev/null
front() { osascript -e 'tell application "System Events" to get name of first process whose frontmost is true' 2>/dev/null; }

before=$(front)
echo "front before: $before"
RECOMP_WINDOW_BACKGROUND=1 RECOMP_MUTE=1 "$PROG" "$SECS" "$@" &
pid=$!
name=$(basename "$PROG")
changed=0
while kill -0 "$pid" 2>/dev/null; do
  now=$(front)
  echo "front during: $now"
  if [ "$now" != "$before" ] || [ "$now" = "$name" ]; then changed=$((changed + 1)); fi
  sleep 0.5
done
wait "$pid"; code=$?
after=$(front)
echo "front after: $after"
[ "$after" != "$before" ] && changed=$((changed + 1))
echo "program exit=$code, front application changed $changed times"
[ "$code" = 0 ] && [ "$changed" = 0 ]
