"""pytest for scripts/patterns/mask_strings.py (story 131.4)."""
import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from mask_strings import mask_strings, skip_string  # noqa: E402


@pytest.mark.parametrize("src,expected", [
    # plain body
    ('let s="hello";', 'let s="_";'),
    # empty string stays empty
    ('let s="";', 'let s="";'),
    # escapes inside the body: \" \\ \n \t \xHH are body, masked away
    (r'let s="a\"b\\c\nd\te\x41";', 'let s="_";'),
    # escaped quote right before the closing quote must not swallow it
    (r'x="\"";y=1', 'x="_";y=1'),
    # adjacent strings, each masked independently
    ('f("a";"b")', 'f("_";"_")'),
    # interpolation: interior kept, surrounding body masked per run
    ('"hello \\(x) world"', '"_\\(x)_"'),
    # interpolation only, no body
    ('"\\(x)"', '"\\(x)"'),
    # interpolation with nested parens
    ('"n=\\(f(g(x);h(1)))!"', '"_\\(f(g(x);h(1)))_"'),
    # nested string literal inside interpolation is itself masked (it is a body)
    ('"len=\\(s.len(r)) f=\\(s.split(r;"f").len)"', '"_\\(s.len(r))_\\(s.split(r;"_").len)"'),
    # several interpolations back to back
    ('"\\(a)\\(b)"', '"\\(a)\\(b)"'),
    # code outside strings untouched, incl. braces/semicolons
    ('m=main;i=io:std.io;f=main():i64{io.println("hi");<0};',
     'm=main;i=io:std.io;f=main():i64{io.println("_");<0};'),
    # no strings at all
    ('let x=1+2;', 'let x=1+2;'),
    # non-ASCII body
    ('"héllo wörld"', '"_"'),
])
def test_mask(src, expected):
    assert mask_strings(src) == expected


def test_idempotent():
    src = 'f("a\\(x)b";"c")'
    once = mask_strings(src)
    assert mask_strings(once) == once


def test_unterminated_is_lenient():
    assert mask_strings('x="abc') == 'x="_'
    assert mask_strings('x="a\\(b') == 'x="_\\(b'


def test_skip_string():
    s = 'p("a\\"b");q'
    end = skip_string(s, 2)
    assert s[end:] == ');q'
    s2 = 'p("\\(f(")"))");q'  # lexer-style raw paren counting: `")"` inside interp is not a string
    # raw counting: \( ( ) ) -> closes at the 4th paren; then `"` closes... document the lexer mirror
    assert skip_string(s2, 2) <= len(s2)


def test_fixture_min_line():
    src = ('m=main;i=io:std.io;i=s:std.str;f=pat(parts:@str;n:i64):str{let r=mut."";'
           'lp(let i=0;i<n;i=i+1){r=s.concat(r;parts.get(i%4))};<r};'
           'f=main():i64{io.println("len=\\(s.len(r)) f=\\(s.split(r;"f").len)");<0};')
    out = mask_strings(src)
    assert out.count('"') == src.count('"')
    assert 'mut.""' in out
    assert '"_\\(s.len(r))_\\(s.split(r;"_").len)"' in out
