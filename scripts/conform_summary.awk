# conform_summary.awk -- the count line `make conform` prints beside each
# passing test/conform script (scripts/conform_isolated.sh).
#
# A script ends with one of two count lines:
#     Results: 12 passed, 0 failed
#     C696: 6 passed, 0 failed          (its own ID, then the counts)
# The last such line wins.  Before 127.269 only the first form was read, so
# a script of the second form was reported with an empty count.
#
# Story: 127.269
/^Results:/ { l = $0 }
/^[A-Z][0-9][0-9][0-9][a-z]?: [0-9]+ passed/ { l = $0 }
END { print l }
