#!/usr/bin/env python3
"""python_subset_gbnf.py — generate and check bench/mask_cost/python-subset.gbnf (story 131.52).

The mask-cost benchmark needs a MAINSTREAM PROGRAMMING-LANGUAGE grammar that the
same engine can load next to toke.gbnf.  No GBNF grammar for Python ships with
XGrammar, llama.cpp or llguidance, and CPython's own grammar is a PEG over an
indentation-tracking tokenizer, which no CFG engine loads.  This script writes
a character-level, context-free GBNF for a SUBSET of Python 3.12+, in the same
style as toke.gbnf (lexer encoded in the grammar, keyword-excluding identifier
trie, required separators between word tokens), so the two are comparable.

What the subset covers: every construct `tkc --python-view` emits for the
benchmark programs (def, class, if/elif/else, while, for, return, match/case
with class and capture patterns, assignment, annotated and augmented
assignment, import, f-strings with nested expressions (PEP 701)), plus the
common rest of the statement and expression language: try/except/finally,
with, raise, del, assert, global/nonlocal, pass/break/continue, lambda,
conditional expressions, comprehensions, dict/set/list/tuple displays,
slices, *args/**kwargs, decorators, raw strings.

What it leaves out (the subset is an under-approximation of Python):
  * indentation other than 4 spaces per level, and nesting deeper than
    --depth levels (default 12): a CFG cannot count, so each level is a copy
    of the statement rules;
  * line continuations (backslash, or a newline inside brackets), byte strings, string prefixes other than r/u/f, async/await/yield,
    walrus, starred assignment targets, `from . import`, f-string `!r` / `=`,
    triple-quoted f-strings and raw strings (plain triple-quoted strings are in).
  * adjacent string literals need a space between them (written without it,
    an empty string followed by another one opens a triple-quoted string);
  * keyword operators need a space on both sides (`not(x)`, `f()if c else d`
    are Python but not in the subset), so word tokens never run together.

The claim this script checks (`--check`, also run by `make bench-mask-cost-check`):
  1. COVERAGE  every `tkc --python-view` output in the benchmark program set
     is accepted (or the script says which are not);
  2. SOUNDNESS grammar accepts F  =>  CPython's ast.parse(F) succeeds, over
     those outputs and deterministic single-edit mutants of them (a delete,
     an insert, a swap per seed per round).  ast.parse is the syntax-only
     analogue of `tkc --dump-ast`: compile-time checks such as `break` outside
     a loop are not syntax.
  3. AGREEMENT the two tuned forms accept exactly what python-subset.gbnf accepts,
     over the hand-written ACCEPTS cases, the traps, the seeds and the mutants.
     --mutation scores this: it deletes each alternative of the tuned forms in
     turn and counts the deletions some input notices (131.149).
Recogniser: XGrammar (the engine the benchmark measures), in the venv of
bench/mask_cost/requirements.txt.

Usage:
  python_subset_gbnf.py --write [--depth N]   regenerate python-subset.gbnf
  python_subset_gbnf.py --check [--rounds R]  regenerate-and-compare, no rule or terminal
                                              unreachable from root (131.151), coverage, soundness
  python_subset_gbnf.py --mutation [--json F] the agreement check's mutation score (131.149)
"""
from __future__ import annotations

import argparse
import ast
import random
import sys
import warnings
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE / "python-subset.gbnf"
DEPTH = 12
# 131.141: the same grammar in the two engine-tuned forms toke gets
# (scripts/gbnf_tune.py): lexeme-level Lark for llguidance, and a GBNF with
# inlined whitespace and word-end lookahead assertions for XGrammar.
LEXEME_OUT = HERE / "python-subset-lexeme.lark"
XGR_OUT = HERE / "python-subset-xgr.gbnf"
sys.path.insert(0, str(HERE.parents[1] / "scripts"))
PY_LEXICAL = ["ws", "ws1", "blank", "nl", "name", "capname", "number", "str", "strq", "tstr",
              "tstrq", "fchunk", "fchunkq", "fspec", "fspecq"]
# (131.151: a lexical rule the lexeme form only uses inside another lexeme --
# tstr/tstrq inside pstr -- gets no terminal of its own there;
# gbnf_tune.lexeme_lark omits it, and refuses to emit an unreachable terminal)
# language-equivalent rewrites of the f-string rules: a run of literal
# f-string characters is one lexeme, and so is a format spec
PY_REFACTOR = r"""
fstr    ::= "f\"" (fchunk | "{{" | "}}" | field)* "\""
fchunk  ::= (fc | fesc)+
field   ::= "{" (ws1 ternary | ternary-nb) ws fspec? "}"
fspec   ::= ":" [^{}\"\\\n\r\x00]*
fstrq   ::= "f'" (fchunkq | "{{" | "}}" | fieldq)* "'"
fchunkq ::= (fcq | fesc)+
fieldq  ::= "{" (ws1 ternary | ternary-nb) ws fspecq? "}"
fspecq  ::= ":" [^{}'\\\n\r\x00]*
"""
# 131.148: llguidance caps the Earley items one mask may create (step_max_items,
# default 50,000), and the 131.141 form exceeded it on 40 of 48 benchmark
# programs.  A mask walks the token trie and adds a parser row wherever a lexeme
# ends inside a token, so the cost is (rows) x (items per row).  These rewrites
# describe the same language with fewer, larger lexemes and fewer predicted
# alternatives per row:
#   * the precedence chain bor .. power (six left-associative levels) is one
#     level, `factor (binop factor)*`, and or/and one level: precedence shapes
#     the parse tree, not the set of strings accepted;
#   * a choice among fixed strings is one regex lexeme: the operator sets
#     (binsym, cmpsym, augop), the plain string kinds (pstr), None/True/False
#     (kwconst), number-or-ellipsis (atomn), pass/break/continue (jump);
#   * the two assignment forms that start with a single target share it;
#   * the blank lines before a line and its indentation are one lexeme per
#     level (ind<k>, see _depth_rules), so neither a run of blank lines nor the
#     dedent continuations of every enclosing level is a row of its own.
# The gate (--check) measures the claim: both forms must agree on every trap,
# seed and mutant.
PY_LEXEME_LEXICAL = ["binsym", "cmpsym", "augop", "pstr", "kwconst", "atomn", "jump"]
PY_LEXEME_REFACTOR = r"""
binsym      ::= "|" | "^" | "&" | "<<" | ">>" | [-+] | "*" | "/" | "//" | "%" | "@" | "**"
cmpsym      ::= "<" | ">" | "==" | ">=" | "<=" | "!="
binop       ::= ws binsym ws
cmpop       ::= ws cmpsym ws | ws1 ("in" | "not" ws1 "in" | "is" | "is" ws1 "not") ws1
boolop      ::= ws1 ("or" | "and") ws1
ortest      ::= nottest (boolop nottest)*
bor         ::= factor (binop factor)*
factor      ::= [-+~] ws factor | primary
ortest-nb   ::= nottest-nb (boolop nottest)*
bor-nb      ::= factor-nb (binop factor)*
factor-nb   ::= [-+~] ws factor | primary-nb
kwconst     ::= "None" | "True" | "False"
pstr        ::= tstr | tstrq | str | strq
string      ::= pstr | fstr | fstrq
atomnb      ::= name | strings | kwconst
              | "(" ws (exprlist ws | test comp ws)? ")"
              | "[" ws (exprlist ws | test comp ws)? "]"
jump        ::= "pass" | "break" | "continue"
single      ::= name | tprimary
target      ::= single | "(" ws targets ws ")" | "[" ws targets ws "]"
small       ::= jump | "return" (ws1 exprlist)?
              | "raise" (ws1 test (ws1 "from" ws1 test)?)?
              | "import" ws1 dotted (ws1 "as" ws1 name)? (ws "," ws dotted (ws1 "as" ws1 name)?)*
              | "from" ws1 dotted ws1 "import" ws1 ("*" | name (ws1 "as" ws1 name)? (ws "," ws name (ws1 "as" ws1 name)?)*)
              | ("global" | "nonlocal") ws1 name (ws "," ws name)*
              | "del" ws1 targets
              | "assert" ws1 test (ws "," ws test)?
              | (targets ws "=" ws)+ exprlist
              | single ws (augop ws exprlist | ":" ws test (ws "=" ws exprlist)?)
              | exprlist
"""
PY_INLINE = {"ws": "[ \\t]*", "ws1": "[ \\t]+"}
PY_WORD_END = "[^0-9A-Za-z_]"
PY_LOOKAHEAD = {"name": PY_WORD_END, "capname": PY_WORD_END, "number": PY_WORD_END}

KEYWORDS = sorted("""False None True and as assert async await break class continue def
del elif else except finally for from global if import in is lambda nonlocal not or
pass raise return try while with yield""".split())

ID_START = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_")
ID_CONT = ID_START | set("0123456789")


def _cls(chars: set[str]) -> str:
    """A GBNF character class for a set of ASCII identifier characters."""
    o = sorted(ord(c) for c in chars)
    parts, i = [], 0
    while i < len(o):
        j = i
        while j + 1 < len(o) and o[j + 1] == o[j] + 1:
            j += 1
        a, b = chr(o[i]), chr(o[j])
        parts.append(a if i == j else (a + b if j == i + 1 else f"{a}-{b}"))
        i = j + 1
    return "[" + "".join(parts) + "]"


def name_trie(rule: str, excluded: list[str]) -> list[str]:
    """Rules for `rule` ::= an identifier that is not in `excluded`: a trie with
    one node per proper prefix of an excluded word, like toke.gbnf's `word`."""
    words = set(excluded)
    prefixes = {w[:k] for w in words for k in range(1, len(w) + 1)}
    out: list[str] = []

    def node(prefix: str) -> str:
        # lower-case rule names (Lark needs them): an upper-case letter X is
        # spelled x0; no keyword contains a digit, so names stay unique
        return rule + ("-" + "".join(c.lower() + "0" if c.isupper() else c for c in prefix) if prefix else "")
    for prefix in [""] + sorted(prefixes):
        pool = ID_START if prefix == "" else ID_CONT
        children = sorted(c for c in pool if prefix + c in prefixes)
        alts = []
        if prefix and prefix not in words:
            alts.append('""')                     # the identifier ends here
        rest = pool - set(children)
        if rest:
            alts.append(f"{_cls(rest)} idc*")      # leaves the trie: any identifier
        alts += [f'"{c}" {node(prefix + c)}' for c in children]
        out.append(f"{node(prefix):<14}::= " + " | ".join(alts))
    return out


def _chain(sfx: str, atom: str) -> list[str]:
    """The precedence chain from `ternary` down to `primary`, for a variant
    whose leftmost atom is `atom` (sfx '' = any; '-nb' = may not start with
    `{`, used right after an f-string's opening brace)."""
    s = sfx
    return [
        f"ternary{s}    ::= ortest{s} (ws1 \"if\" ws1 ortest ws1 \"else\" ws1 test)?",
        f"ortest{s}     ::= andtest{s} (ws1 \"or\" ws1 andtest)*",
        f"andtest{s}    ::= nottest{s} (ws1 \"and\" ws1 nottest)*",
        f"nottest{s}    ::= \"not\" ws1 nottest | comparison{s}",
        f"comparison{s} ::= bor{s} (cmpop bor)*",
        f"bor{s}        ::= bxor{s} (ws \"|\" ws bxor)*",
        f"bxor{s}       ::= band{s} (ws \"^\" ws band)*",
        f"band{s}       ::= shift{s} (ws \"&\" ws shift)*",
        f"shift{s}      ::= arith{s} (ws (\"<<\" | \">>\") ws arith)*",
        f"arith{s}      ::= term{s} (ws [-+] ws term)*",
        f"term{s}       ::= factor{s} (ws (\"*\" | \"/\" | \"//\" | \"%\" | \"@\") ws factor)*",
        f"factor{s}     ::= [-+~] ws factor | power{s}",
        f"power{s}      ::= primary{s} (ws \"**\" ws factor)?",
        f"primary{s}    ::= {atom} trailer* | atomn",
    ]


def _strings(q: str, tag: str) -> list[str]:
    """Plain, raw and f-string rules for one quote character."""
    qq = '\\"' if q == '"' else q
    return [
        f"str{tag}     ::= \"u\"? \"{qq}\" (sc{tag} | esc)* \"{qq}\""
        f" | \"r\" \"{qq}\" (sc{tag} | \"\\\\\" [^\\n\\r\\x00])* \"{qq}\"",
        f"sc{tag}      ::= [^{qq}\\\\\\n\\r\\x00]",
        f"tstr{tag}    ::= \"u\"? \"{qq}{qq}{qq}\" ((\"{qq}\" \"{qq}\"?)? (tc{tag} | tesc))* \"{qq}{qq}{qq}\"",
        f"tc{tag}      ::= [^{qq}\\\\\\r\\x00]",
        f"fstr{tag}    ::= \"f\" \"{qq}\" (fc{tag} | fesc | \"{{{{\" | \"}}}}\" | field{tag})* \"{qq}\"",
        f"fc{tag}      ::= [^{qq}\\\\\\n\\r\\x00{{}}]",
        f"field{tag}   ::= \"{{\" ([ \\t]+ ternary | ternary-nb) ws (\":\" [^{{}}{qq}\\\\\\n\\r\\x00]*)? \"}}\"",
    ]


def _depth_rules(d: int, depth: int, lexeme: bool = False) -> list[str]:
    """The statement rules of indentation level `d`.  With `lexeme` (131.148,
    the llguidance form only): the blank lines before a line and its
    indentation are ONE lexeme, ind<k> ::= blank* "<4k spaces>", instead of a
    `blank*` loop of one-line lexemes and an indentation literal.  The same
    strings; see PY_LEXEME_REFACTOR."""
    def at(k: int) -> str:
        """What precedes a keyword or statement written at level k."""
        if lexeme:
            return f"ind{k} "
        return "blank* " + (f'"{"    " * k}" ' if k else "")
    ind = "    " * d
    i = f'"{ind}" ' if d else ""
    nxt = d + 1
    deeper = nxt <= depth
    suite = f"suite{d}"
    lead = f"ind{d} " if lexeme and d else i
    r = [
        f"stmt{d}      ::= {lead}(simples nl | compound{d})",
        f"compound{d}  ::= if{d} | while{d} | for{d} | def{d} | class{d} | try{d} | with{d} | deco{d}"
        + (f" | match{d}" if deeper else ""),
        f"{suite}     ::= ws \":\" ws simples nl" + (f" | ws \":\" nl block{nxt}" if deeper else ""),
        f"if{d}        ::= \"if\" ws1 test {suite} ({at(d)}\"elif\" ws1 test {suite})* ({at(d)}\"else\" {suite})?",
        f"while{d}     ::= \"while\" ws1 test {suite} ({at(d)}\"else\" {suite})?",
        f"for{d}       ::= \"for\" ws1 targets ws1 \"in\" ws1 exprlist {suite} ({at(d)}\"else\" {suite})?",
        f"def{d}       ::= \"def\" ws1 name ws \"(\" ws (params ws)? \")\" (ws \"->\" ws test)? {suite}",
        f"class{d}     ::= \"class\" ws1 name (ws \"(\" ws (cargs ws)? \")\")? {suite}",
        f"try{d}       ::= \"try\" {suite} (({at(d)}except{d})+ ({at(d)}\"except\" {suite})?"
        f" | {at(d)}\"except\" {suite}) ({at(d)}\"else\" {suite})? ({at(d)}\"finally\" {suite})?"
        f" | \"try\" {suite} {at(d)}\"finally\" {suite}",
        f"except{d}    ::= \"except\" ws1 test (ws1 \"as\" ws1 name)? {suite}",
        f"with{d}      ::= \"with\" ws1 witem (ws \",\" ws witem)* {suite}",
        f"deco{d}      ::= (\"@\" ws test nl {at(d)})+ (def{d} | class{d})",
    ]
    if deeper:
        r.append(f"match{d}     ::= \"match\" ws1 exprlist ws \":\" nl ({at(nxt)}case{nxt})+")
        r.append(f"case{nxt}      ::= \"case\" ws1 pattern (ws1 \"if\" ws1 test)? suite{nxt}")
    if deeper:
        r.append(f"block{nxt}     ::= stmt{nxt}+" if lexeme
                 else f"block{nxt}     ::= blank* stmt{nxt} (blank* stmt{nxt})*")
    return r


def generate(depth: int = DEPTH) -> str:
    rules: list[str] = []
    for d in range(depth + 1):
        rules += _depth_rules(d, depth)
    head = f"""\
# Python 3.12+ SUBSET -- GBNF for the mask-cost benchmark (story 131.52).
#
# GENERATED by bench/mask_cost/python_subset_gbnf.py --depth {depth} -- do not edit.
# Character level (the tokenizer is encoded, as in docs/spec/toke.gbnf):
# 4-space indentation, nesting to {depth} levels (one copy of the statement
# rules per level), keyword-excluding identifiers.  An under-approximation of
# Python: what it accepts, CPython's ast.parse accepts (checked by
# `python_subset_gbnf.py --check`); see the generator's docstring for what is
# left out.  This is a benchmark input, not a toke artefact.

root        ::= (blank | stmt0)*
blank       ::= [ \\t]* ("#" [^\\n\\r\\x00]*)? "\\n"
nl          ::= ws ("#" [^\\n\\r\\x00]*)? "\\n"
ws          ::= [ \\t]*
ws1         ::= [ \\t]+
"""
    body = f"""
# -- simple statements --
simples     ::= small (ws ";" ws small)* (ws ";")?
small       ::= "pass" | "break" | "continue" | "return" (ws1 exprlist)?
              | "raise" (ws1 test (ws1 "from" ws1 test)?)?
              | "import" ws1 dotted (ws1 "as" ws1 name)? (ws "," ws dotted (ws1 "as" ws1 name)?)*
              | "from" ws1 dotted ws1 "import" ws1 ("*" | name (ws1 "as" ws1 name)? (ws "," ws name (ws1 "as" ws1 name)?)*)
              | ("global" | "nonlocal") ws1 name (ws "," ws name)*
              | "del" ws1 targets
              | "assert" ws1 test (ws "," ws test)?
              | (targets ws "=" ws)+ exprlist
              | (name | tprimary) ws augop ws exprlist
              | (name | tprimary) ws ":" ws test (ws "=" ws exprlist)?
              | exprlist
augop       ::= "+=" | "-=" | "*=" | "/=" | "//=" | "%=" | "**=" | ">>=" | "<<=" | "&=" | "|=" | "^=" | "@="
dotted      ::= name (ws "." ws name)*
targets     ::= target (ws "," ws target)* (ws ",")?
target      ::= name | tprimary | "(" ws targets ws ")" | "[" ws targets ws "]"
tprimary    ::= atom trailer* (ws "." ws name | ws "[" ws subs ws "]")
witem       ::= test (ws1 "as" ws1 target)?

# -- parameters and arguments --
params      ::= pnd (ws "," ws pnd)* (ws "," ws pdf)* (ws "," ws ptail)? (ws ",")?
              | pdf (ws "," ws pdf)* (ws "," ws ptail)? (ws ",")?
              | ptail (ws ",")?
pnd         ::= name (ws ":" ws test)?
pdf         ::= name (ws ":" ws test)? ws "=" ws test
ptail       ::= "*" ws name (ws ":" ws test)? (ws "," ws (pdf | pnd))* (ws "," ws "**" ws name (ws ":" ws test)?)?
              | "**" ws name (ws ":" ws test)?
lparams     ::= name (ws "," ws name)* (ws "," ws lpdf)* (ws ",")? | lpdf (ws "," ws lpdf)* (ws ",")?
lpdf        ::= name ws "=" ws test
args        ::= cargs | test comp
cargs       ::= parg (ws "," ws parg)* (ws "," ws karg)* (ws ",")? | karg (ws "," ws karg)* (ws ",")?
parg        ::= test | "*" ws test
karg        ::= name ws "=" ws test | "**" ws test

# -- expressions --
test        ::= ternary | lambdef
lambdef     ::= "lambda" (ws1 lparams)? ws ":" ws test
exprlist    ::= test (ws "," ws test)* (ws ",")?
cmpop       ::= ws ("<" | ">" | "==" | ">=" | "<=" | "!=") ws
              | ws1 ("in" | "not" ws1 "in" | "is" | "is" ws1 "not") ws1
{chr(10).join(_chain("", "atom"))}
{chr(10).join(_chain("-nb", "atomnb"))}
trailer     ::= ws "(" ws (args ws)? ")" | ws "[" ws subs ws "]" | ws "." ws name
subs        ::= sub (ws "," ws sub)* (ws ",")?
sub         ::= test | (ternary ws)? ":" (ws ternary)? (ws ":" (ws ternary)?)?
atom        ::= atomnb | "{{" ws (dictitems ws | exprlist ws | kv comp ws | test comp ws)? "}}"
atomnb      ::= name | strings | "None" | "True" | "False"
              | "(" ws (exprlist ws | test comp ws)? ")"
              | "[" ws (exprlist ws | test comp ws)? "]"
atomn       ::= number | "..."
dictitems   ::= kv (ws "," ws kv)* (ws ",")?
kv          ::= ternary ws ":" ws test | "**" ws bor
comp        ::= (ws1 "for" ws1 targets ws1 "in" ws1 ortest (ws1 "if" ws1 ortest)*)+

# -- match patterns --
pattern     ::= orpat (ws1 "as" ws1 capname)?
orpat       ::= closedpat (ws "|" ws closedpat)*
closedpat   ::= "-"? number | str | strq | "None" | "True" | "False"
              | capname | "_" | name (ws "." ws name)+
              | "(" ws pattern ws ")" | "(" ws ")"
              | "(" ws pattern ws "," (ws pattern (ws "," ws pattern)* (ws ",")?)? ws ")"
              | "[" ws (pattern (ws "," ws pattern)* (ws ",")? ws)? "]"
              | name (ws "." ws name)* ws "(" ws (classargs ws)? ")"
classargs   ::= pattern (ws "," ws pattern)* (ws "," ws kwpat)* (ws ",")? | kwpat (ws "," ws kwpat)* (ws ",")?
kwpat       ::= name ws "=" ws pattern

# -- lexical --
strings     ::= string (ws1 string)*
string      ::= tstr | tstrq | str | strq | fstr | fstrq
{chr(10).join(_strings('"', ""))}
{chr(10).join(_strings("'", "q"))}
esc         ::= "\\\\" ([^xNuU\\n\\r\\x00] | "x" hex hex | "u" hex hex hex hex)
tesc        ::= "\\\\" ([^xNuU\\r\\x00] | "x" hex hex | "u" hex hex hex hex)
fesc        ::= "\\\\" ([^xNuU\\n\\r\\x00{{}}] | "x" hex hex | "u" hex hex hex hex)
hex         ::= [0-9a-fA-F]
number      ::= "0"+ | [1-9] [0-9]* | "0" [xX] hex+ | "0" [bB] [01]+ | "0" [oO] [0-7]+
              | [0-9]+ "." [0-9]+ ([eE] [-+]? [0-9]+)? | [0-9]+ [eE] [-+]? [0-9]+
idc         ::= [0-9A-Z_a-z]
"""
    tries = name_trie("name", KEYWORDS) + name_trie("capname", KEYWORDS + ["_"])
    return head + "\n# -- statements, one copy per indentation level --\n" + "\n".join(rules) + "\n" + body \
        + "# keyword-excluding identifiers (a trie over the 35 hard keywords; capname also excludes `_`)\n" \
        + "\n".join(tries) + "\n"


def generate_lexeme(depth: int = DEPTH) -> str:
    from gbnf_tune import lexeme_lark
    head = (f"// Python 3.12+ SUBSET -- LEXEME-level Lark for llguidance (stories 131.141, 131.148).\n"
            f"// GENERATED by bench/mask_cost/python_subset_gbnf.py --depth {depth} from\n"
            f"// python-subset.gbnf (scripts/gbnf_tune.py lexeme_lark) -- do not edit.\n"
            f"// The same language; the lexical rules are regex lexemes, the precedence\n"
            f"// chain is flattened and blank lines plus indentation are one lexeme per\n"
            f"// level, so that a mask fits llguidance's default limits.  A benchmark input.\n\n")
    stmts = [r for d in range(depth + 1) for r in _depth_rules(d, depth, lexeme=True)]
    inds = [f"ind{d} ::= blank*" + (f' "{"    " * d}"' if d else "") for d in range(depth + 1)]
    # (blank | stmt0)* with each run of blank lines one lexeme
    refactor = "\n".join([PY_REFACTOR, PY_LEXEME_REFACTOR, "root ::= ind0 (stmt0 ind0)*"] + stmts + inds) + "\n"
    lexical = PY_LEXICAL + PY_LEXEME_LEXICAL + [f"ind{d}" for d in range(depth + 1)]
    return lexeme_lark(generate(depth), lexical, 1, head, refactor)


def generate_xgr(depth: int = DEPTH) -> str:
    from gbnf_tune import xgrammar_tuned
    head = (f"# Python 3.12+ SUBSET -- GBNF tuned for XGrammar (story 131.141).\n"
            f"# GENERATED by bench/mask_cost/python_subset_gbnf.py --depth {depth} from\n"
            f"# python-subset.gbnf (scripts/gbnf_tune.py xgrammar_tuned) -- do not edit.\n"
            f"# The same language: whitespace inlined, word-end lookahead assertions.\n\n")
    return xgrammar_tuned(generate(depth), PY_INLINE, PY_LOOKAHEAD, head)


def tuned_acceptors() -> dict:
    """{label: acceptor} for the two tuned forms (engines required)."""
    from gbnf_tune import llg_acceptor, xgr_acceptor
    return {"llguidance:python-subset-lexeme.lark": llg_acceptor(LEXEME_OUT.read_text(encoding="utf-8")),
            "xgrammar:python-subset-xgr.gbnf": xgr_acceptor(XGR_OUT.read_text(encoding="utf-8"))}


# ----------------------------------------------------------------- check ----
def _ast_ok(text: str) -> bool:
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        try:
            ast.parse(text)
            return True
        except (SyntaxError, ValueError, MemoryError, RecursionError):
            return False


def mutants(text: str, rnd: random.Random, rounds: int) -> list[str]:
    alphabet = list(" \t\n()[]{}:;,.=+-*/%<>!&|^~@#'\"\\_aefinorstxyz019")
    out = []
    for _ in range(rounds):
        if len(text) < 2:
            break
        k = rnd.randrange(len(text))
        out.append(text[:k] + text[k + 1:])
        k = rnd.randrange(len(text) + 1)
        out.append(text[:k] + rnd.choice(alphabet) + text[k:])
        k = rnd.randrange(len(text) - 1)
        out.append(text[:k] + text[k + 1] + text[k] + text[k + 2:])
    return out


# Hand-written traps: inputs a careless character-level grammar accepts and
# CPython rejects.  Each must be rejected by the grammar (or accepted by
# ast.parse); they join the mutants in the soundness count.
TRAPS = [
    'x = f"{{a:1}[a]}"\n',          # `{{` is an escape, not a field opening on a dict
    'x = ""' + '"a"\n',              # an empty string glued to a string opens a triple quote
    'x = f"{lambda: 1}"\n',         # lambda needs parentheses in an f-string field
    'x = r"\\"\n',                 # a raw string cannot end in an odd backslash
    'x = "\\N{DASH}"\n',           # \N{...} must name a character
    'x = "\\x4"\n',                # truncated \x escape
    'x = 01\n',                     # leading zero in a decimal integer
    'x = 1.e\n',                    # a float cannot take an attribute directly
    'f(**k, a)\n',                  # positional argument after **
    'f(a=1, b)\n',                  # positional argument after keyword
    'f(x for x in y, z)\n',         # unparenthesised generator with another argument
    'class A(x for x in y):\n    pass\n',
    'del f()\n', 'f() = 1\n', 'f() += 1\n', 'x: int, y = 1\n',
    'def f(a=1, b):\n    pass\n',  # non-default after default
    'if = 1\n', 'x = not\n', 'x = a not b\n', 'x = a == not b\n',
    'try:\n    pass\nexcept:\n    pass\nexcept E:\n    pass\n',  # bare except not last
    'match x:\n    case _ as _:\n        pass\n',
    'match x:\n    case f"a":\n        pass\n',
    'for f() in x:\n    pass\n',
    'if x:\n  y = 1\n',            # two-space indentation (outside the subset, and fine either way)
    'if x:\n    y = 1\n  z = 2\n',  # dedent to an unknown level
]


# 131.149: hand-written ACCEPT cases.  The traps above are inputs the grammar
# must reject; these are inputs it must accept, written so that every
# alternative of the tuned forms is exercised (no `tkc --python-view` seed
# contains `**`, a decorator or `nonlocal`).  `--mutation` measures the claim:
# it deletes each alternative of the tuned forms in turn and counts the
# deletions the agreement check notices.  Every case must be accepted by
# python-subset.gbnf and by ast.parse (--check fails otherwise).
_OPS = "a | b ^ c & d << e >> f + g - h * i / j // k % l @ m ** n"
_CMP = "a < b > c == d >= e <= f != g in h not in i is j is not k"
_ESC = r"\n\x41\u0041"
# an f-string field starts a second copy of the precedence chain, and each of
# its levels is reached only by a field whose loosest operator is at that level
_FOPS = "".join("{a %s b}" % op for op in "| ^ & << >> + - * / // % @ **".split())
ACCEPTS = [
    # every binary, comparison, boolean and unary operator
    f"x = {_OPS}\n",
    f"x = {_CMP}\n",
    "x = not a or b and not c\nx = -a + +b * ~c\nx = a**-b\nx = (a)(b)\n",
    # augmented and annotated assignment, every operator and target form
    "".join(f"x {op}= 1\n" for op in "+ - * / // % ** >> << & | ^ @".split())
    + "x.a += 1\nx[0] -= 1\nx: int\nx: int = 1\nx.a: int = 1\nx[0]: int\n",
    # constants and every number form
    "x = (None, True, False, ...)\n"
    "x = (0, 00, 7, 10, 0x1F, 0XaB, 0b10, 0B01, 0o17, 0O7, 1.5, 1.5e3, 1.5E-3, 1.5e+3, 1e9, 1E-9)\n",
    # every string kind, prefix and escape; adjacent strings
    f'x = ("a{_ESC}", u"b", r"c\\q", \'a{_ESC}\', u\'b\', r\'c\\q\', "")\n'
    f'x = ("""a{_ESC}\n"b""c""", u"""""", \'\'\'a{_ESC}\n\'b\'\'c\'\'\', u\'\'\'\'\'\')\n'
    'x = "a" \'b\' """c""" f"d"\n',
    # f-strings: escapes, doubled braces, specs, and every operator in a field
    f'x = f"a{_ESC}{{{{b}}}}{{c}}{{ d }}{{e:>10}}{{ {{1}}}}"\n'
    f"x = f'a{_ESC}{{{{b}}}}{{c}}{{ d }}{{e:>10}}{{ {{1}}}}'\n",
    f'x = f"{{{_OPS}}}{{{_CMP}}}{{not a}}{{-a}}{{1}}{{a or b and c}}{{a if b else c}}{{"s"}}{_FOPS}"\n'
    f"x = f'{{{_OPS}}}{{{_CMP}}}{{not a}}{{-a}}{{1}}{{a or b and c}}{{a if b else c}}{{\"s\"}}{_FOPS}'\n",
    # displays, comprehensions, subscripts and slices, trailers
    "x = [a, b,]\nx = []\nx = ()\nx = (a,)\nx = {}\nx = {a, b}\nx = {a: b, **c}\n"
    "x = {a: b for a in c}\nx = {a for a in b}\nx = [a for a in b if c if d for e in f]\n"
    "x = (a for a in b)\n",
    "x = a[b]\nx = a[b:c]\nx = a[:]\nx = a[::]\nx = a[b:c:d]\nx = a[:c]\nx = a[::d]\n"
    "x = a[b, c:d]\nx = a[b,]\nx = a.b(c)(d)[e]\n",
    # lambda, conditional expression, calls
    "f = lambda: 0\nf = lambda a, b=1: a\nf = lambda a=1, b=2,: a\nf = lambda a,: a\n"
    "x = a if b else c\nx = lambda: (yield_)\n",
    "f(a, *b, c=1, **d)\nf(**d)\nf(c=1,)\nf(a for a in b)\nf()\nf(a,)\n",
    # parameters
    "def f(a, b: int, c=1, d: int = 2, *e: int, g, h: int = 3, **i: int) -> int: pass\n"
    "def f(**k): pass\ndef f(a=1, *b, **c,): pass\ndef f(a,): pass\ndef f(*a, b=1): pass\n",
    # simple statements
    "import a\nimport a.b as c, d\nfrom a import *\nfrom a.b import c as d, e\n"
    "global a, b\nnonlocal a\ndel a, b.c, d[0], (e, f), [g]\nassert a\nassert a, b\n"
    "raise\nraise a\nraise a from b\nreturn\nreturn a, b\npass; break; continue;\n"
    "a = b = c\na, b = c\n(a, b), [c] = d\nfor (a, b), [c] in d: pass\n",
    # compound statements with every continuation, blank and comment lines between them
    "with a: pass\nwith a as b, c as (d, e): pass\n\n# comment\n"
    "@a\n@b.c(d)\n\ndef f(): pass\n@a\nclass C(B, metaclass=M): pass\nclass D(): pass\n"
    "try: pass\nexcept E as e: pass\n\nexcept (F, G): pass\nexcept: pass\nelse: pass\nfinally: pass\n",
    # every pattern form
    "match x:\n"
    "    case 0 | 00 | 7 | -1 | 0x1F | 0b1 | 0o7 | 1.5 | 1.5e3 | 1e9 | -1.5: pass\n"
    f'    case "a{_ESC}" | u"b" | r"c\\q" | \'a{_ESC}\' | u\'b\' | r\'c\\q\': pass\n'
    "    case None | True | False: pass\n"
    "    case a.b | a.b.c: pass\n"
    "    case (y) | (): pass\n"
    "    case (y,) | (y, z,) | (y, z): pass\n"
    "    case [] | [y] | [y, z,]: pass\n"
    "    case A() | A.B(y, k=z) | A(k=z,) | A(y,): pass\n"
    "    case [_x, y] as w if w: pass\n"
    "    case _:\n        pass\n",
]


def _names() -> list[str]:
    """One identifier per alternative of the keyword-excluding identifier
    trie: every proper keyword prefix that is not a keyword, and every trie
    node followed by a character that leaves the trie."""
    prefixes = {w[:k] for w in KEYWORDS for k in range(1, len(w) + 1)}
    out = []
    for p in [""] + sorted(prefixes):
        pool = ID_START if p == "" else ID_CONT
        rest = sorted(pool - {c for c in pool if p + c in prefixes})
        if p and p not in KEYWORDS:
            out.append(p)
        if rest:
            out.append(p + rest[0])
            out.append(p + rest[-1])
    return sorted(set(out))


def _depth_program(d: int, depth: int = DEPTH) -> str:
    """Every compound statement, each continuation and both suite forms at
    indentation level `d`, reached through `d` nested `if`s."""
    lines = ["    " * k + "if x:" for k in range(d)]
    body = ["if a: pass", "elif b: pass", "else: pass",
            "while a: pass", "else: pass", "for a in b: pass", "else: pass",
            "def f(): pass", "class C: pass", "with a: pass",
            "try: pass", "except E: pass", "except: pass", "else: pass", "finally: pass",
            "try: pass", "except: pass", "try: pass", "finally: pass",
            "@d", "def f(): pass", "@d", "class C: pass"]
    if d < depth:
        body += ["if a:", "    pass", "match x:", "    case _: pass"]
    ind = "    " * d
    return "\n".join(lines + [ind + s for s in body]) + "\n"


def accept_cases(depth: int = DEPTH) -> list[tuple[str, str]]:
    names = _names()
    out = [(f"ACCEPTS[{k}]", t) for k, t in enumerate(ACCEPTS)]
    out.append(("ACCEPTS[names]", "x = [" + ", ".join(names) + "]\n"
                + "match x:\n    case [" + ", ".join(n for n in names) + "]: pass\n"))
    out += [(f"ACCEPTS[depth{d}]", _depth_program(d, depth)) for d in range(depth + 1)]
    return out


def agreement_set(texts: list[tuple[str, str]], rounds: int, seed: int = 131052,
                  accepts: bool = True) -> list[tuple[str, str, str]]:
    """The agreement inputs, in the order --check runs them: [(label, text, kind)],
    kind one of accept / trap / seed / mutant.  `accepts=False` leaves out the
    131.149 accept cases (the set before that story)."""
    rnd = random.Random(seed)
    out = [(lbl, t, "accept") for lbl, t in accept_cases()] if accepts else []
    out += [(f"TRAPS[{k}]", t, "trap") for k, t in enumerate(TRAPS)]
    for label, t in texts:
        out.append((label, t, "seed"))
        out += [(label, m, "mutant") for m in mutants(t, rnd, rounds)]
    return out


def check(texts: list[tuple[str, str]], rounds: int, seed: int = 131052,
          tuned: dict | None = None) -> dict:
    """texts: [(label, python source)].  Returns the coverage/soundness report.
    `tuned` ({label: acceptor}): forms that must agree with python-subset.gbnf
    on every accept case, trap, seed and mutant (131.141, 131.149)."""
    import xgrammar as xgr
    from xgrammar.testing import _is_grammar_accept_string
    g = xgr.Grammar.from_ebnf(OUT.read_text(encoding="utf-8"), root_rule_name="root")
    acc = lambda s: bool(_is_grammar_accept_string(g, s))  # noqa: E731
    rep = {"seeds": len(texts), "seeds_accepted": 0, "seeds_rejected": [],
           "mutants": 0, "mutants_grammar_accepts": 0, "mutants_ast_accepts": 0,
           "unsound": [], "rounds": rounds, "seed": seed, "traps": len(TRAPS),
           "accepts": 0, "accepts_rejected": [],
           "tuned": {k: {"checked": 0, "disagree": []} for k in (tuned or {})}}

    def agree(label: str, t: str, a: bool) -> None:
        for k, f in (tuned or {}).items():
            r = rep["tuned"][k]
            r["checked"] += 1
            if f(t) != a:
                r["disagree"].append({"seed": label, "text": t, "subset": a})
    earley = None
    for label, t, kind in agreement_set(texts, rounds, seed):
        a = acc(t)
        agree(label, t, a)
        if kind == "accept":
            # in the subset as XGrammar, gbnf_earley and CPython read it
            if earley is None:
                from gbnf_earley import Grammar
                earley = Grammar(OUT.read_text(encoding="utf-8"))
            rep["accepts"] += 1
            if not (a and earley.accepts(t) and _ast_ok(t)):
                rep["accepts_rejected"].append(label)
        elif kind == "trap":
            if a and not _ast_ok(t):
                rep["unsound"].append({"seed": label, "text": t})
        elif kind == "seed":
            if a:
                rep["seeds_accepted"] += 1
            else:
                rep["seeds_rejected"].append(label)
        else:
            p = _ast_ok(t)
            rep["mutants"] += 1
            rep["mutants_grammar_accepts"] += a
            rep["mutants_ast_accepts"] += p
            if a and not p:
                rep["unsound"].append({"seed": label, "text": t})
    return rep


def mutation(texts: list[tuple[str, str]], rounds: int, out_json: Path | None = None,
             jobs: int | None = None, forms: list[str] | None = None) -> int:
    """131.149: the agreement check's mutation score.  Each alternative of each
    alternation in the two tuned forms is deleted in turn; a deletion is killed
    when some agreement input's verdict differs from python-subset.gbnf's.
    Scored on the set without the accept cases (before) and with them (after)."""
    import json
    import time
    import xgrammar as xgr
    from xgrammar.testing import _is_grammar_accept_string
    from gbnf_earley import Grammar
    from gbnf_tune import llg_acceptor, xgr_acceptor
    from grammar_mutation import mutants as alt_mutants, print_report, score, summarise
    g = xgr.Grammar.from_ebnf(OUT.read_text(encoding="utf-8"), root_rule_name="root")
    full = agreement_set(texts, rounds)
    reference = [bool(_is_grammar_accept_string(g, t)) for _, t, _ in full]
    acc_n = sum(k == "accept" for _, _, k in full)
    # the accept cases must be in the subset as gbnf_earley reads it, too
    t0 = time.time()
    earley = Grammar(OUT.read_text(encoding="utf-8"))
    bad = [lbl for lbl, t, k in full[:acc_n] if not earley.accepts(t)]
    print(f"[mutation] {acc_n} accept cases: gbnf_earley on python-subset.gbnf accepts "
          f"{acc_n - len(bad)}/{acc_n} ({time.time() - t0:.0f}s)")
    if bad or not all(reference[:acc_n]):
        print(f"[mutation] FAIL: accept cases outside the subset: {bad}")
        return 1
    before = [(lbl, t) for lbl, t, _ in full[acc_n:]]
    new = [(lbl, t) for lbl, t, _ in full[:acc_n]]
    dump = {}
    for tag, path, dialect, mk in (("llguidance:python-subset-lexeme.lark", LEXEME_OUT, "lark", llg_acceptor),
                                   ("xgrammar:python-subset-xgr.gbnf", XGR_OUT, "gbnf", xgr_acceptor)):
        if forms and dialect not in forms:
            continue
        text = path.read_text(encoding="utf-8")
        ms = alt_mutants(text, dialect)
        t0 = time.time()
        rep0 = score(ms, text, mk, before, reference[acc_n:], progress=True, jobs=jobs)
        print(f"[mutation] {tag}: before 131.149 ({len(before)} inputs, {time.time() - t0:.0f}s)")
        print_report(f"[mutation] {tag} before:", rep0, show=0)
        # a survivor ran every old input, so only the accept cases can kill it
        t0 = time.time()
        live = [m for m in ms if m.status == "survived"]
        score(live, text, mk, new, reference[:acc_n], jobs=jobs)
        rep1 = summarise(ms)
        print(f"[mutation] {tag}: after ({len(before) + len(new)} inputs, {time.time() - t0:.0f}s)")
        print_report(f"[mutation] {tag} after:", rep1, show=100)
        dump[tag] = {"before": rep0, "after": rep1}
    if out_json:
        out_json.write_text(json.dumps(dump, indent=1), encoding="utf-8")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--mutation", action="store_true",
                   help="the agreement check's mutation score (131.149; slow)")
    ap.add_argument("--json", type=Path, default=None, help="--mutation: write the report here")
    ap.add_argument("--jobs", type=int, default=None, help="--mutation: worker processes")
    ap.add_argument("--forms", default="lark,gbnf",
                    help="--mutation: which tuned forms (lark = llguidance, gbnf = XGrammar)")
    ap.add_argument("--depth", type=int, default=DEPTH)
    ap.add_argument("--rounds", type=int, default=3)
    a = ap.parse_args(argv)
    outs = ((OUT, generate), (LEXEME_OUT, generate_lexeme), (XGR_OUT, generate_xgr))
    if a.write:
        for path, gen in outs:
            path.write_text(gen(a.depth), encoding="utf-8")
            print(f"wrote {path}")
        return 0
    stale = False
    for path, gen in outs:
        if path.read_text(encoding="utf-8") != gen(a.depth):
            print(f"FAIL: {path.name} is not the generation (run --write)")
            stale = True
    # 131.151: the tuned forms carry no rule or terminal unreachable from root
    from gbnf_tune import dead_check
    if any([dead_check(p, "[python-subset]") for p in (LEXEME_OUT, XGR_OUT)]) or stale:
        return 1
    sys.path.insert(0, str(HERE))
    from mask_cost import program_set  # noqa: E402
    sel: dict = {}
    progs = program_set(sel)
    if a.mutation:
        return mutation([(p["path"], p["python"]) for p in progs], a.rounds, a.json, a.jobs,
                        a.forms.split(","))
    rep = check([(p["path"], p["python"]) for p in progs], a.rounds, tuned=tuned_acceptors())
    # program_set drops what this grammar rejects; coverage counts those drops
    rejected = sel["excluded"]["python_subset_rejects"] + rep["seeds_rejected"]
    seen = len(progs) + len(sel["excluded"]["python_subset_rejects"])
    print(f"[python-subset] coverage: {seen - len(rejected)}/{seen} python-view outputs that "
          f"ast.parse accepts are accepted")
    for lbl in rejected[:10]:
        print(f"    rejected: {lbl}")
    print(f"[python-subset] soundness: {rep['traps']} traps + {rep['mutants']} mutants, grammar accepts "
          f"{rep['mutants_grammar_accepts']}, ast.parse accepts {rep['mutants_ast_accepts']}, "
          f"grammar-only {len(rep['unsound'])}")
    print(f"[python-subset] accept cases: {rep['accepts'] - len(rep['accepts_rejected'])}/"
          f"{rep['accepts']} accepted by python-subset.gbnf (XGrammar and gbnf_earley) and ast.parse")
    for lbl in rep["accepts_rejected"]:
        print(f"    NOT ACCEPTED: {lbl}")
    for u in rep["unsound"][:5]:
        print(f"    UNSOUND ({u['seed']}): {u['text'][:300]!r}")
    for k, r in rep["tuned"].items():
        print(f"[python-subset] {k}: {r['checked'] - len(r['disagree'])}/{r['checked']} "
              f"accept cases, traps, seeds and mutants agree with python-subset.gbnf")
        for d in r["disagree"][:5]:
            print(f"    DISAGREES ({d['seed']}, subset {'accepts' if d['subset'] else 'rejects'}): "
                  f"{d['text'][:200]!r}")
    ok = (not rejected and not rep["unsound"] and not rep["accepts_rejected"]
          and not any(r["disagree"] for r in rep["tuned"].values()))
    print("[python-subset] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
