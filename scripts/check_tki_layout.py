#!/usr/bin/env python3
"""check_tki_layout.py — a C struct that mirrors a .tki record has its layout.

Story 138.18.  tkc lowers a record as one 8-byte slot per field, in the order
the module's .tki declares the fields, and reads `x.f` as slot <index of f>
whatever f's type.  A C struct that the glue hands to toke code as a record,
or reads back from it, must therefore have:

  * the .tki's fields, by name, in the .tki's order, and no others;
  * every field 8 bytes wide at byte offset 8 * index;
  * every array-typed field ([T], [[T]], ...) declared int64_t -- a toke
    array handle, never an inline {data, len} pair or a pointer type.

std.http's `req` was read through the server's C Req, whose fields are in
another order and whose pair lists are 16-byte {data, len} pairs; req.body
read the headers pointer.  This check is what would have said so.

A struct opts in with a marker comment on the line before its typedef:

    /* tki-record: http.req */
    typedef struct { ... } TkReqRec;

A C struct that carries private fields after the record's (http's
HttpClient, HttpStream) uses `tki-record-prefix:` instead; its leading fields
are checked and the rest are its own.  C names compare without underscores
or case (C's base_url is the .tki's baseurl, 113.2a).

Markers are read from src/stdlib/*.h (a record crosses a file boundary, so
it belongs in a header).  The names and order are compared in Python; the
offsets and sizes are asserted by the C compiler itself, on a generated
translation unit that includes the header, so the check measures the struct
the glue is compiled with, not a re-parse of it.

Every record a .tki function RETURNS (its success type, or the element
type of an array it returns) must be marked (135.30): such a record is a
block the glue built and toke reads by slot, so an unmarked one is a layout
nobody checks -- llm's $llmclient was one, and `.model` printed the API key.
Records with no fields are exempt (no field can be read).  Records owned by
another batch's territory that are not marked yet are listed in PENDING
below with the story that owns them; they are reported, not failed, and an
entry whose record has since been marked, or that no longer names a
returned record, fails (the list only shrinks).

Exit 0 when every marked struct agrees with its record and every returned
record is marked or pending; exit 1 otherwise, naming the struct, the
record and the difference; exit 1 also when no marker is found at all (a
check that finds nothing to check has checked nothing).

    python3 scripts/check_tki_layout.py            # the repo's src/stdlib
    python3 scripts/check_tki_layout.py --list     # also print what it checked
"""

import glob
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STDLIB_C = os.path.join(ROOT, "src", "stdlib")
STDLIB_TKI = os.path.join(ROOT, "stdlib")

MARKER = re.compile(r"/\*\s*tki-record(-prefix)?:\s*([A-Za-z0-9_]+)\.([A-Za-z0-9_$]+)")
TYPEDEF_END = re.compile(r"^\}\s*([A-Za-z_][A-Za-z0-9_]*)\s*;")
FIELD = re.compile(r"^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*;$")


# Returned records not marked yet, each owned outside story 135.30's
# territory (src/stdlib for llm, canvas, chart, dashboard, db dbrow, html,
# i18n, ml, svg, template, tls, ws).  "mod.type": "owner / why".  Delete an
# entry when its struct is marked; a stale entry fails the check.
PENDING = {
    "analytics.groupstat":    "135.30 residue: analytics glue",
    "analytics.statsrow":     "135.30 residue: analytics glue",
    "analytics.tspoint":      "135.30 residue: analytics glue",
    "auth.jwtclaims":         "135.30 residue: auth glue",
    "csv.csvdialect":         "135.30 residue: csv glue",
    "csv.csvopts":            "135.30 residue: csv glue",
    "csv.csvreader":          "135.30 residue: csv glue",
    "csv.csvrow":             "135.30 residue: csv glue",
    "csv.csvwriter":          "135.30 residue: csv glue",
    "dataframe.dataframe":    "135.30 residue: dataframe glue",
    "dataframe.dfshape":      "135.30 residue: dataframe glue",
    "dataframe.series":       "135.30 residue: dataframe glue",
    "encrypt.decryptresult":  "135.30 residue: encrypt glue",
    "encrypt.keypair":        "135.30 residue: encrypt glue",
    "image.imgbuf":           "135.30 residue: image glue",
    "infer.modelhandle":      "135.30 residue: infer glue",
    "json.jsonval":           "135.30 residue: json glue (LS / HX)",
    "json.jsonstream":        "135.30 residue: json stream glue (HX)",
    "json.jsonwriter":        "135.30 residue: json glue (LS)",
    "llmtool.toolcall":       "135.30 residue: llm_tool_glue.c (GH / LH)",
    "math.linregresult":      "135.30 residue: math glue",
    "mdns.discovered":        "135.30 residue: mdns glue",
    "mlx.mlxmodel":           "135.30 residue: mlx glue",
    "router.router":          "135.30 residue: router_glue.c (LH)",
    "time.timeduration":      "135.30 residue: time glue",
    "time.timeparts":         "135.30 residue: time glue",
    "toon.toonval":           "135.30 residue: toon glue",
    "vecstore.veccollection": "135.30 residue: vecstore glue",
    "vecstore.vecstorehandle": "135.30 residue: vecstore glue",
    "yaml.yamlval":           "135.30 residue: yaml glue",
}


def returned_records():
    """{(module, record): [function, ...]} for every record with fields that
    a .tki function returns, directly or as an array's elements."""
    tkis, records = {}, set()
    for path in sorted(glob.glob(os.path.join(STDLIB_TKI, "*.tki"))):
        mod = os.path.basename(path)[:-4]
        try:
            with open(path, encoding="utf-8") as f:
                tkis[mod] = json.load(f)
        except (OSError, ValueError):
            continue            # an unreadable .tki is other checks' business
        for e in tkis[mod].get("exports", []):
            if e.get("kind") == "type" and not e.get("is_sum") and e.get("fields"):
                records.add((mod, e.get("name")))
    out = {}
    for mod, d in tkis.items():
        for e in d.get("exports", []):
            if e.get("kind") != "func":
                continue
            t = str(e.get("return", "")).split("!", 1)[0].strip()
            t = re.sub(r"^\?\((.*)\)$", r"\1", t).lstrip("$")
            if t.startswith("[") and t.endswith("]"):
                t = t[1:-1].lstrip("$")
            key = tuple(t.split(".", 1)) if "." in t else (mod, t)
            if key in records:
                out.setdefault(key, []).append(e.get("name"))
    return out


def strip_comments(line):
    line = re.sub(r"/\*.*?\*/", "", line)
    return line.split("//", 1)[0].strip()


def find_marked_structs():
    """[(header, lineno, module, record, struct_name, [(ctype, field)], prefix)]"""
    out, errors = [], []
    for path in sorted(glob.glob(os.path.join(STDLIB_C, "*.h"))):
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        i = 0
        while i < len(lines):
            m = MARKER.search(lines[i])
            if not m:
                i += 1
                continue
            prefix, mod, rec, at = bool(m.group(1)), m.group(2), m.group(3), i + 1
            j = i + 1
            while j < len(lines) and not lines[j].strip().startswith("typedef"):
                if "*/" in lines[j] or lines[j].strip().startswith("*") or not lines[j].strip():
                    j += 1
                    continue
                break
            if j >= len(lines) or not lines[j].strip().startswith("typedef struct"):
                errors.append("%s:%d: tki-record marker %s.%s is not followed by "
                              "a typedef struct" % (rel(path), at, mod, rec))
                i = j
                continue
            fields, name = [], None
            k = j + 1
            while k < len(lines):
                end = TYPEDEF_END.match(lines[k].strip())
                if end:
                    name = end.group(1)
                    break
                body = strip_comments(lines[k])
                if body:
                    fm = FIELD.match(body)
                    if not fm or "," in body or "[" in body:
                        errors.append("%s:%d: cannot read field %r (one plain "
                                      "field per line)" % (rel(path), k + 1, body))
                    else:
                        fields.append((" ".join(fm.group(1).split()), fm.group(2)))
                k += 1
            if not name:
                errors.append("%s:%d: typedef after marker %s.%s never ends"
                              % (rel(path), at, mod, rec))
                break
            out.append((path, at, mod, rec, name, fields, prefix))
            i = k + 1
    return out, errors


def rel(path):
    return os.path.relpath(path, ROOT)


_tki_cache = {}


def tki_record(mod, rec):
    if mod not in _tki_cache:
        path = os.path.join(STDLIB_TKI, mod + ".tki")
        try:
            with open(path, encoding="utf-8") as f:
                _tki_cache[mod] = json.load(f)
        except (OSError, ValueError) as e:
            _tki_cache[mod] = e
    d = _tki_cache[mod]
    if isinstance(d, Exception):
        return None, "cannot read stdlib/%s.tki: %s" % (mod, d)
    for e in d.get("exports", []):
        if e.get("kind") == "type" and e.get("name") == rec:
            if e.get("is_sum"):
                return None, "%s.%s is a sum type, not a record" % (mod, rec)
            return [(fl["name"], fl["type"]) for fl in e.get("fields", [])], None
    return None, "stdlib/%s.tki declares no type %r" % (mod, rec)


def norm(name):
    """C spells a field base_url where the .tki says baseurl (no-underscore
    convention, 113.2a); compare the two without underscores or case."""
    return name.replace("_", "").lower()


def compare_fields(struct, mod, rec, cfields, tfields, prefix):
    errs = []
    if prefix:
        if len(cfields) < len(tfields):
            errs.append("%s has %d fields, fewer than the %d of %s.%s"
                        % (struct, len(cfields), len(tfields), mod, rec))
        cfields = cfields[:len(tfields)]
    cn = [f for _, f in cfields]
    tn = [f for f, _ in tfields]
    if [norm(f) for f in cn] != [norm(f) for f in tn]:
        errs.append("%s mirrors %s.%s but its fields are %s; the .tki's are %s"
                    % (struct, mod, rec, cn, tn))
    tt = dict((norm(f), t) for f, t in tfields)
    for ctype, f in cfields:
        t = tt.get(norm(f), "")
        if t.lstrip("$").startswith("[") and ctype != "int64_t":
            errs.append("%s.%s is %s in .tki %s.%s, an array: it must be an "
                        "int64_t toke array handle, not %r"
                        % (struct, f, t, mod, rec, ctype))
    return errs


def compile_layout(structs):
    """Assert slot offsets/sizes with the C compiler; return error lines.

    One translation unit per header: two stdlib headers may each define a
    helper type of the same name (db.h's and ml.h's F64Result), and are
    never included together by the glue either (135.30)."""
    errors = []
    for header in sorted({p for p, *_ in structs}):
        src = ["#include <stddef.h>", "#include <stdint.h>",
               '#include "%s"' % os.path.basename(header)]
        for path, _, mod, rec, name, fields, prefix in structs:
            if path != header:
                continue
            tfields, err = tki_record(mod, rec)
            if not err and prefix:
                fields = fields[:len(tfields)]
            for i, (_, f) in enumerate(fields):
                src.append("typedef char tki_layout__%s__%s__is_slot_%d[(offsetof(%s, %s) "
                           "== %d && sizeof(((%s *)0)->%s) == 8) ? 1 : -1];"
                           % (name, f, i, name, f, 8 * i, name, f))
            if not prefix:
                src.append("typedef char tki_layout__%s__has_%d_slots[(sizeof(%s) == %d) ? 1 : -1];"
                           % (name, len(fields), name, 8 * len(fields)))
        cc = os.environ.get("CC", "cc")
        with tempfile.TemporaryDirectory() as d:
            tu = os.path.join(d, "tki_layout.c")
            with open(tu, "w") as f:
                f.write("\n".join(src) + "\n")
            r = subprocess.run([cc, "-std=c99", "-D_GNU_SOURCE", "-fsyntax-only",
                                "-I", STDLIB_C, tu],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               universal_newlines=True)
        if r.returncode == 0:
            continue
        names = sorted(set(re.findall(r"tki_layout__[A-Za-z0-9_]+", r.stdout)))
        if not names:
            errors.append("the layout translation unit for %s did not compile:\n%s"
                          % (rel(header), r.stdout))
            continue
        errors += ["C layout: %s" % n.replace("tki_layout__", "").replace("__", " ")
                   + " -- false" for n in names]
    return errors


def main(argv):
    structs, errors = find_marked_structs()
    if not structs and not errors:
        print("check_tki_layout: FAIL -- no tki-record marker in src/stdlib/*.h; "
              "nothing was checked")
        return 1
    for path, at, mod, rec, name, cfields, prefix in structs:
        tfields, err = tki_record(mod, rec)
        if err:
            errors.append("%s:%d: %s" % (rel(path), at, err))
            continue
        for e in compare_fields(name, mod, rec, cfields, tfields, prefix):
            errors.append("%s:%d: %s" % (rel(path), at, e))
    errors += compile_layout(structs)
    marked = {(mod, rec) for _, _, mod, rec, *_ in structs}
    returned = returned_records()
    pending = []
    for (mod, rec), funcs in sorted(returned.items()):
        name = "%s.%s" % (mod, rec)
        if (mod, rec) in marked:
            if name in PENDING:
                errors.append("PENDING lists %s, but it is marked now: delete "
                              "the entry" % name)
        elif name in PENDING:
            pending.append("%s (%s): %s" % (name, ", ".join(funcs[:3]), PENDING[name]))
        else:
            errors.append("%s is returned by %s but no struct in src/stdlib/*.h "
                          "is marked `tki-record: %s` -- nothing checks the "
                          "layout toke reads it by" % (name, ", ".join(funcs[:3]), name))
    for name in sorted(PENDING):
        if tuple(name.split(".", 1)) not in returned:
            errors.append("PENDING lists %s, which no .tki function returns: "
                          "delete the entry" % name)
    if "--list" in argv:
        for path, at, mod, rec, name, cfields, prefix in structs:
            tf, _ = tki_record(mod, rec)
            n = len(tf) if tf else len(cfields)
            print("  %s:%d  %s = %s.%s (%d slots%s)"
                  % (rel(path), at, name, mod, rec, n,
                     ", then private fields" if prefix and len(cfields) > n else ""))
    for p in pending:
        print("check_tki_layout: pending (not marked yet): " + p)
    if errors:
        for e in errors:
            print("check_tki_layout: " + e)
        print("check_tki_layout: FAIL -- %d problem(s) in %d marked struct(s)"
              % (len(errors), len(structs)))
        return 1
    print("check_tki_layout: OK -- %d C struct(s) match their .tki records "
          "slot for slot; %d of %d returned records marked, %d pending"
          % (len(structs), len(returned) - len(pending), len(returned),
             len(pending)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
