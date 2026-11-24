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
Recogniser: XGrammar (the engine the benchmark measures), in the venv of
bench/mask_cost/requirements.txt.

Usage:
  python_subset_gbnf.py --write [--depth N]   regenerate python-subset.gbnf
  python_subset_gbnf.py --check [--rounds R]  regenerate-and-compare, coverage, soundness
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


def check(texts: list[tuple[str, str]], rounds: int, seed: int = 131052,
          tuned: dict | None = None) -> dict:
    """texts: [(label, python source)].  Returns the coverage/soundness report.
    `tuned` ({label: acceptor}): forms that must agree with python-subset.gbnf
    on every trap, seed and mutant (131.141)."""
    import xgrammar as xgr
    from xgrammar.testing import _is_grammar_accept_string
    g = xgr.Grammar.from_ebnf(OUT.read_text(encoding="utf-8"), root_rule_name="root")
    acc = lambda s: bool(_is_grammar_accept_string(g, s))  # noqa: E731
    rep = {"seeds": len(texts), "seeds_accepted": 0, "seeds_rejected": [],
           "mutants": 0, "mutants_grammar_accepts": 0, "mutants_ast_accepts": 0,
           "unsound": [], "rounds": rounds, "seed": seed,
           "tuned": {k: {"checked": 0, "disagree": []} for k in (tuned or {})}}

    def agree(label: str, t: str, a: bool) -> None:
        for k, f in (tuned or {}).items():
            r = rep["tuned"][k]
            r["checked"] += 1
            if f(t) != a:
                r["disagree"].append({"seed": label, "text": t, "subset": a})
    rnd = random.Random(seed)
    rep["traps"] = len(TRAPS)
    for k, t in enumerate(TRAPS):
        a = acc(t)
        agree(f"TRAPS[{k}]", t, a)
        if a and not _ast_ok(t):
            rep["unsound"].append({"seed": f"TRAPS[{k}]", "text": t})
    for label, t in texts:
        a = acc(t)
        agree(label, t, a)
        if a:
            rep["seeds_accepted"] += 1
        else:
            rep["seeds_rejected"].append(label)
        for m in mutants(t, rnd, rounds):
            rep["mutants"] += 1
            a, p = acc(m), _ast_ok(m)
            agree(label, m, a)
            rep["mutants_grammar_accepts"] += a
            rep["mutants_ast_accepts"] += p
            if a and not p:
                rep["unsound"].append({"seed": label, "text": m})
    return rep


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    ap.add_argument("--depth", type=int, default=DEPTH)
    ap.add_argument("--rounds", type=int, default=3)
    a = ap.parse_args(argv)
    outs = ((OUT, generate), (LEXEME_OUT, generate_lexeme), (XGR_OUT, generate_xgr))
    if a.write:
        for path, gen in outs:
            path.write_text(gen(a.depth), encoding="utf-8")
            print(f"wrote {path}")
        return 0
    for path, gen in outs:
        if path.read_text(encoding="utf-8") != gen(a.depth):
            print(f"FAIL: {path.name} is not the generation (run --write)")
            return 1
    sys.path.insert(0, str(HERE))
    from mask_cost import program_set  # noqa: E402
    sel: dict = {}
    progs = program_set(sel)
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
    for u in rep["unsound"][:5]:
        print(f"    UNSOUND ({u['seed']}): {u['text'][:300]!r}")
    for k, r in rep["tuned"].items():
        print(f"[python-subset] {k}: {r['checked'] - len(r['disagree'])}/{r['checked']} traps, "
              f"seeds and mutants agree with python-subset.gbnf")
        for d in r["disagree"][:5]:
            print(f"    DISAGREES ({d['seed']}, subset {'accepts' if d['subset'] else 'rejects'}): "
                  f"{d['text'][:200]!r}")
    ok = not rejected and not rep["unsound"] and not any(r["disagree"] for r in rep["tuned"].values())
    print("[python-subset] " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
