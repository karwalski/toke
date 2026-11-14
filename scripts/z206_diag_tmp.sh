#!/usr/bin/env bash
# TEMPORARY Z206 diagnostics (removed before handoff)
grep TKC_LD_PATH src/build_info.h
W=$(mktemp -d); cd "$W"
cat > g.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int c, char **v) {
  for (int z = 1; z < c; z++) {
    setenv("TZ", v[z], 1); tzset();
    long prev = -99999; 
    for (time_t t = 1730604000; t <= 1730620800; t += 60) {
      struct tm tm; localtime_r(&t, &tm);
      if (tm.tm_gmtoff != prev) { printf("%-28s %ld -> off %ld isdst %d (%02d:%02d local)\n", v[z], (long)t, tm.tm_gmtoff, tm.tm_isdst, tm.tm_hour, tm.tm_min); prev = tm.tm_gmtoff; }
    }
  }
  return 0;
}
C
cc -o g g.c && ./g EST5EDT 'EST5EDT,M3.2.0,M11.1.0' America/New_York
exit 0
