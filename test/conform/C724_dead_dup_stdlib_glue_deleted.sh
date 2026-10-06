#!/usr/bin/env bash
# C724_dead_dup_stdlib_glue_deleted.sh — the duplicate C implementations
# (136.67) and the unreachable glue wrappers (136.71) stay deleted.
#
# Owner decision 2026-10-01 §5.4 / §5.5.  136.67: every `dup` line of
# scripts/core_reach_triage.txt — a second C copy of a surfaced capability
# (file_join beside path.join, str_buf_* beside str.buf, ...).  136.71: the
# `_w` wrappers docs/reference/stdlib-internal.md classed `dead` — nothing in
# src/ calls them and no toke spelling reaches them (tk_math_*_w shadowed by
# the libc mapping, tk_http_res*_w / req*_w / serve*_w shadowed by the
# tk_http_res_* / req_* lowering targets, tk_df_*_w by tk_dataframe_*_w, the
# ratelimit / uuid / validate / tool stubs with no interface entry) — plus
# tk_map_put_w, which only the deleted tk_map_setint_w called.  None of them
# implemented a .tki-quarantined export, so none was wired up instead.
#
# Cases:
#   1 no src/stdlib/*.c or *.h defines or declares a deleted symbol
#   2 src/stdlib_decls_gen.h declares none of them (regenerated)
#   3 check-core-reach: 0 dup, 0 untriaged, 0 stale
#
# Negative control: on main at c2aa3a7a case 1 and 2 are red (every symbol is
# defined, the glue ones declared) and case 3 is red (14 dup lines; one stale).
#
# Story: 136.67, 136.71

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
DUP="auth_apikeygen crypto_from_hex csv_reader_has_next file_basename file_dirname file_ext file_join file_rmdir str_buf_add str_buf_addbyte str_buf_done str_buf_new str_rindex tk_test_assert_true"
DEAD="tk_base64_decode_w tk_df_columnstr_w tk_df_filter_w tk_df_fromcsv_w tk_df_fromrows_w tk_df_groupby_w tk_df_shape_w tk_float_parse_w tk_http_getstatic_w tk_http_getstaticmime_w tk_http_postecho_w tk_http_postjson_w tk_http_poststatic_w tk_http_reqbody_w tk_http_reqheader_w tk_http_reqmethod_w tk_http_reqparam_w tk_http_reqpath_w tk_http_resbad_w tk_http_reserr_w tk_http_resjson_w tk_http_resnew_w tk_http_resok_w tk_http_serve_w tk_http_servedir_w tk_http_servetls_w tk_http_setcors_w tk_http_setnotfound_w tk_map_getint_w tk_map_setint_w tk_math_ceil_w tk_math_exp_w tk_math_floor_w tk_math_log10_w tk_math_log_w tk_math_pow_w tk_math_round_w tk_math_sqrt_w tk_ratelimit_check_w tk_ratelimit_new_w tk_soap12_envelope_w tk_sort_ints_w tk_sort_strs_w tk_tool_call_w tk_tool_register_w tk_tpl_load_w tk_tpl_render_w tk_uuid_v4_w tk_validate_email_w tk_validate_range_w tk_validate_url_w tk_map_put_w"

found=""
for s in ${DUP} ${DEAD}; do
    if /usr/bin/grep -l -E "(^|[^A-Za-z0-9_])${s}[[:space:]]*\(" "${REPO_ROOT}"/src/stdlib/*.c "${REPO_ROOT}"/src/stdlib/*.h >/dev/null 2>&1; then
        found="${found} ${s}"
    fi
done
expect "1-not-defined" "${found:-none}" "none"

found=""
for s in ${DUP} ${DEAD}; do
    /usr/bin/grep -q "\"${s}\"" "${REPO_ROOT}/src/stdlib_decls_gen.h" && found="${found} ${s}"
done
expect "2-not-declared" "${found:-none}" "none"

out="$(cd "${REPO_ROOT}" && python3 scripts/report_core_reach.py --check 2>&1)"; rc=$?
dup="$(printf '%s\n' "${out}" | awk '$1=="dup"{print $2}')"
unt="$(printf '%s\n' "${out}" | awk '$1=="untriaged"{print $2}')"
stl="$(printf '%s\n' "${out}" | awk '$1=="stale"{print $2}')"
expect "3-core-reach" "rc=${rc} dup=${dup} untriaged=${unt} stale=${stl}" "rc=0 dup=0 untriaged=0 stale=0"

echo "C724: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
