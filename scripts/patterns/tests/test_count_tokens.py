"""pytest for count_tokens.py region isolation (story 131.4). Needs ./tkc."""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import count_tokens as ct  # noqa: E402

pytestmark = pytest.mark.skipif(not ct.TKC.exists(), reason="tkc binary not built")

PROG = ('m=main;\ni=io:std.io;\nf=pat(x:i64):str{\n  let s="héllo \\(x) w}rld";\n  <s\n};\n'
        'f=main():i64{\n  io.println(pat(1));\n  <0\n};\n')


def test_function_extent_excludes_harness_and_handles_utf8_and_brace_in_string():
    m = ct.min_text_of(PROG)
    s, e = ct.function_extent(m, "pat")
    assert m[s:e] == 'f=pat(x:i64):str{let s="héllo \\(x) w}rld";<s}'
    s2, e2 = ct.function_extent(m, "main")
    assert m[s2:e2] == 'f=main():i64{io.println(pat(1));<0}'


def test_missing_function_raises():
    m = ct.min_text_of(PROG)
    with pytest.raises(RuntimeError):
        ct.function_extent(m, "nope")


def test_full_span_and_brace_fallback_agree(monkeypatch):
    """131.31: a full FUNC_DECL span is used as is; an old tkc's one-token
    span falls back to brace matching.  Both give the same extent."""
    m = ct.min_text_of(PROG)
    got = {n: ct.function_extent(m, n) for n in ("pat", "main")}
    real = ct._dump_ast_text

    def one_token_spans(text):
        ast = real(text)

        def cut(n):
            if n.get("kind") == "FUNC_DECL":
                n["span"]["end"] = n["span"]["start"] + 1
            for c in n.get("children") or []:
                cut(c)
        cut(ast)
        return ast
    monkeypatch.setattr(ct, "_dump_ast_text", one_token_spans)
    for n in ("pat", "main"):
        assert ct.function_extent(m, n) == got[n]
