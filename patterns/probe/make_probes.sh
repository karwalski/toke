#!/usr/bin/env bash
# Epic 131.2 — generates one complete toke program per probe (<name>.tk) plus its
# expected stdout (<name>.expect). Expected output comes from the syntax-card
# known-good forms (the oracle); for symbols the card does not define, the
# natural semantic is stated in combinator-status.md. A first line of
# "#checkfail" in .expect means correctness == `tkc --check` REJECTS the program.
set -euo pipefail
cd "$(dirname "$0")"
rm -f ./*.tk ./*.expect
P='m=probe;i=io:std.io;i=s:std.str;'
PA='m=probe;i=io:std.io;i=s:std.str;i=arr:std.array;'
H='f=dbl(x:i64):i64{<x*2};f=iseven(x:i64):bool{<x%2==0};f=addf(acc:i64;x:i64):i64{<acc+x};f=cmp(a:i64;b:i64):i64{<a-b};f=pr(x:i64):i64{io.println("\(x)");<0};'
mk() { # name expect body   (body is the main() body; prelude chosen by $PRE)
  printf '%s%s\nf=main():i64{%s<0};\n' "$PRE" "$H" "$3" > "$1.tk"
  printf '%b\n' "$2" > "$1.expect"
}
A='let a=@(3;1;2);'
# ── arrays, method / UFCS style ────────────────────────────────────────────
PRE=$P
mk map_method      '6 2 4'   "${A}let b=a.map(&dbl);io.println(\"\\(b.get(0)) \\(b.get(1)) \\(b.get(2))\");"
mk filter_method   '1 2'     "${A}let b=a.filter(&iseven);io.println(\"\\(b.len) \\(b.get(0))\");"
mk find_method     '2'       "${A}io.println(\"\\(a.find(2))\");"
mk all_method      '0'   "${A}io.println(\"\\(a.all(&iseven))\");"
mk any_method      '1'    "${A}io.println(\"\\(a.any(&iseven))\");"
mk count_method    '1'       "${A}io.println(\"\\(a.count(&iseven))\");"
mk sum_method      '6'       "${A}io.println(\"\\(a.sum())\");"
mk min_method      '1'       "${A}io.println(\"\\(a.min())\");"
mk max_method      '3'       "${A}io.println(\"\\(a.max())\");"
mk sort_method     '1 2 3'   "${A}let b=a.sort(&cmp);io.println(\"\\(b.get(0)) \\(b.get(1)) \\(b.get(2))\");"
mk first_method    '3'       "${A}io.println(\"\\(a.first())\");"
mk last_method     '2'       "${A}io.println(\"\\(a.last())\");"
mk indexof_method  '1'       "${A}io.println(\"\\(a.indexof(1))\");"
mk contains_method '1'    "${A}io.println(\"\\(a.contains(2))\");"
mk slice_method    '1 2'     "${A}let b=a.slice(1;3);io.println(\"\\(b.get(0)) \\(b.get(1))\");"
mk reduce_method   '6'       "${A}io.println(\"\\(a.reduce(0;&addf))\");"
mk fold_method     '6'       "${A}io.println(\"\\(a.fold(0;&addf))\");"
mk each_method     '3\n1\n2' "${A}a.each(&pr);"
mk keys_prop       '2 a'     'let m=@("a":1;"b":2);let k=m.keys;io.println("\(k.len) \(k.get(0))");'
mk keys_method     '2 a'     'let m=@("a":1;"b":2);let k=m.keys();io.println("\(k.len) \(k.get(0))");'
mk values_prop     '2 1'     'let m=@("a":1;"b":2);let v=m.values;io.println("\(v.len) \(v.get(0))");'
mk values_method   '2 1'     'let m=@("a":1;"b":2);let v=m.values();io.println("\(v.len) \(v.get(0))");'
mk append_method   '4 9'     "let a=mut.@(3;1;2);a=a.append(9);io.println(\"\\(a.len) \\(a.get(3))\");"
mk push_method     '4 9'     "let a=mut.@(3;1;2);a=a.push(9);io.println(\"\\(a.len) \\(a.get(3))\");"
mk push_bare       '3'       "let a=mut.@(3;1;2);a.push(9);io.println(\"\\(a.len)\");"
mk push_bare_empty '0'       "let a=mut.@();a.push(9);io.println(\"\\(a.len)\");"
mk pop_method      '2 3'     "${A}let b=a.pop();io.println(\"\\(b.len) \\(b.get(0))\");"
mk set_method      '7'       "let a=mut.@(3;1;2);a=a.set(0;7);io.println(\"\\(a.get(0))\");"
mk set_bare        '3'       "let a=mut.@(3;1;2);a.set(0;7);io.println(\"\\(a.get(0))\");"
mk get_method      '1'       "${A}io.println(\"\\(a.get(1))\");"
mk get_const       '1'       "${A}io.println(\"\\(a.1)\");"
mk len_prop        '3'       "${A}io.println(\"\\(a.len)\");"
mk len_method      '3'       "${A}io.println(\"\\(a.len())\");"
mk arrconcat_op    '4 9'     "let a=mut.@(3;1;2);a=a+@(9);io.println(\"\\(a.len) \\(a.get(3))\");"
# ── arrays, module style via i=arr:std.array ───────────────────────────────
PRE=$PA
mk map_module      '6 2 4'   "${A}let b=arr.map(a;&dbl);io.println(\"\\(b.get(0)) \\(b.get(1)) \\(b.get(2))\");"
mk filter_module   '1 2'     "${A}let b=arr.filter(a;&iseven);io.println(\"\\(b.len) \\(b.get(0))\");"
mk find_module     '2'       "${A}io.println(\"\\(arr.find(a;2))\");"
mk all_module      '0'   "${A}io.println(\"\\(arr.all(a;&iseven))\");"
mk any_module      '1'    "${A}io.println(\"\\(arr.any(a;&iseven))\");"
mk count_module    '1'       "${A}io.println(\"\\(arr.count(a;&iseven))\");"
mk sum_module      '6'       "${A}io.println(\"\\(arr.sum(a))\");"
mk min_module      '1'       "${A}io.println(\"\\(arr.min(a))\");"
mk max_module      '3'       "${A}io.println(\"\\(arr.max(a))\");"
mk sort_module     '1 2 3'   "${A}let b=arr.sort(a;&cmp);io.println(\"\\(b.get(0)) \\(b.get(1)) \\(b.get(2))\");"
mk first_module    '3'       "${A}io.println(\"\\(arr.first(a))\");"
mk last_module     '2'       "${A}io.println(\"\\(arr.last(a))\");"
mk indexof_module  '1'       "${A}io.println(\"\\(arr.indexof(a;1))\");"
mk contains_module '1'    "${A}io.println(\"\\(arr.contains(a;2))\");"
mk slice_module    '1 2'     "${A}let b=arr.slice(a;1;3);io.println(\"\\(b.get(0)) \\(b.get(1))\");"
mk reduce_module   '6'       "${A}io.println(\"\\(arr.reduce(a;0;&addf))\");"
mk fold_module     '6'       "${A}io.println(\"\\(arr.fold(a;0;&addf))\");"
mk each_module     '3\n1\n2' "${A}arr.each(a;&pr);"
mk append_module   '4 9'     "let a=mut.@(3;1;2);a=arr.append(a;9);io.println(\"\\(a.len) \\(a.get(3))\");"
mk push_module     '4 9'     "let a=mut.@(3;1;2);a=arr.push(a;9);io.println(\"\\(a.len) \\(a.get(3))\");"
mk pop_module      '2 3'     "${A}let b=arr.pop(a);io.println(\"\\(b.len) \\(b.get(0))\");"
mk set_module      '7'       "let a=mut.@(3;1;2);a=arr.set(a;0;7);io.println(\"\\(a.get(0))\");"
mk get_module      '1'       "${A}io.println(\"\\(arr.get(a;1))\");"
mk len_module      '3'       "${A}io.println(\"\\(arr.len(a))\");"
mk join_module_arr 'a-b'     'let p=@("a";"b");io.println(arr.join(p;"-"));'
# ── strings: join / split / fields ─────────────────────────────────────────
PRE=$P
mk join_method     'a-b'     'let p=@("a";"b");io.println(p.join("-"));'
mk join_module     'a-b'     'let p=@("a";"b");io.println(s.join("-";p));'
mk join_swapped    '#checkfail' 'let p=@("a";"b");io.println(s.join(p;"-"));'
mk split_method    '3 b'     'let w="a,b,c".split(",");io.println("\(w.len) \(w.get(1))");'
mk split_module    '3 b'     'let w=s.split("a,b,c";",");io.println("\(w.len) \(w.get(1))");'
mk fields_method   '3 b'     'let w="a b  c".fields();io.println("\(w.len) \(w.get(1))");'
mk fields_module   '3 b'     'let w=s.fields("a b  c");io.println("\(w.len) \(w.get(1))");'
mk fields_interp   'a'       'let x="a b  c";io.println("\(s.fields(x).get(0))");'
mk fields_get_direct 'a'     'let x="a b  c";io.println(s.fields(x).get(0));'
mk split_interp    'b'       'let x="a,b,c";io.println("\(s.split(x;",").get(1))");'
# ── strings: len / upper / lower / ends / starts / replace / contains / indexof / slice / find
mk strlen_prop     '6'       'let x="abcdef";io.println("\(x.len)");'
mk strlen_method   '6'       'let x="abcdef";io.println("\(x.len())");'
mk strlen_module   '6'       'let x="abcdef";io.println("\(s.len(x))");'
mk strlen_lit_prop '6'       'io.println("\("abcdef".len)");'
mk upper_method    'AB'      'let x="ab";io.println(x.upper());'
mk upper_module    'AB'      'let x="ab";io.println(s.upper(x));'
mk lower_method    'ab'      'let x="AB";io.println(x.lower());'
mk lower_module    'ab'      'let x="AB";io.println(s.lower(x));'
mk ends_method     '1'    'let x="ab";io.println("\(x.ends("b"))");'
mk endswith_module '1'    'let x="ab";io.println("\(s.endswith(x;"b"))");'
mk starts_method   '1'    'let x="ab";io.println("\(x.starts("a"))");'
mk startswith_module '1'  'let x="ab";io.println("\(s.startswith(x;"a"))");'
mk replace_method  'a+b'     'let x="a-b";io.println(x.replace("-";"+"));'
mk replace_module  'a+b'     'let x="a-b";io.println(s.replace(x;"-";"+"));'
mk strcontains_method '1' 'let x="abc";io.println("\(x.contains("b"))");'
mk strcontains_module '1' 'let x="abc";io.println("\(s.contains(x;"b"))");'
mk strindexof_method '2'     'let x="abc";io.println("\(x.indexof("c"))");'
mk strindexof_module '2'     'let x="abc";io.println("\(s.indexof(x;"c"))");'
mk strslice_method 'bc'      'let x="abcdef";io.println(x.slice(1;3));'
mk strslice_module 'bc'      'let x="abcdef";io.println(s.slice(x;1;3));'
mk strfind_method  '2'       'let x="abc";io.println("\(x.find("c"))");'
mk strfind_module  '2'       'let x="abc";io.println("\(s.find(x;"c"))");'
mk trim_method     'a'       'let x="  a  ";io.println(x.trim());'
mk trim_module     '[a]'     'let x="  a  ";io.println("[\(s.trim(x))]");'
mk concat_method   'ab'      'let x="a";io.println(x.concat("b"));'
mk concat_module   'ab'      'let x="a";io.println(s.concat(x;"b"));'
# ── interpolation of method-call results (127.7 / 127.9) ───────────────────
mk interp_trim_method   '[a]' 'let x="  a  ";io.println("[\(x.trim())]");'
mk interp_trim_let      '[a]' 'let x="  a  ";let y=x.trim();io.println("[\(y)]");'
mk interp_trim_module   '[a]' 'let x="  a  ";io.println("[\(s.trim(x))]");'
mk interp_concat_method 'ab'  'let x="a";io.println("\(x.concat("b"))");'
mk interp_slice_method  'bc'  'let x="abcdef";io.println("\(x.slice(1;3))");'
mk interp_upper_module  'AB'  'let x="ab";io.println("\(s.upper(x))");'
mk interp_split_get     'b'   'let x="a,b,c";io.println("\(x.split(",").get(1))");'
mk interp_replace_module 'a+b' 'let x="a-b";io.println("\(s.replace(x;"-";"+"))");'
# ── UFCS arity (127.1) ─────────────────────────────────────────────────────
mk ufcs_extra_arg   '#checkfail' 'let a="xx";let c=a.concat("A";"B");io.println(c);'
mk ufcs_extra_arg_module '#checkfail' 'let a="xx";let c=s.concat(a;"A";"B");io.println(c);'
mk user_fn_extra_arg '#checkfail' 'let c=dbl(1;2);io.println("\(c)");'
# ── interpolation-built strings stored via append (127.10) ─────────────────
mk interp_append_loop '0: item\n1: item\n2: item' 'let items=mut.@();lp(let i=0;i<3;i=i+1){let line="\(i): item";items=items.append(line)};lp(let j=0;j<3;j=j+1){io.println(items.get(j))};'
mk interp_append_direct '0: item\n1: item\n2: item' 'let items=mut.@();lp(let i=0;i<3;i=i+1){items=items.append("\(i): item")};lp(let j=0;j<3;j=j+1){io.println(items.get(j))};'
mk concat_append_loop '0: item\n1: item\n2: item' 'let items=mut.@();lp(let i=0;i<3;i=i+1){let line=s.concat(s.fromint(i);": item");items=items.append(line)};lp(let j=0;j<3;j=j+1){io.println(items.get(j))};'
mk interp_append_interp_read '0: item\n1: item\n2: item' 'let items=mut.@();lp(let i=0;i<3;i=i+1){let line="\(i): item";items=items.append(line)};lp(let j=0;j<3;j=j+1){io.println("\(items.get(j))")};'
mk interp_append_dfio '1: a\n2: b' 'let lines=s.split("a\nb";"\n");let labels=mut.@();lp(let i=0;i<lines.len;i=i+1){labels=labels.append("\(i+1)")};let tagged=mut.@();lp(let i=0;i<lines.len;i=i+1){tagged=tagged.append("\(labels.get(i)): \(lines.get(i))")};io.println(s.join("\n";tagged));'
mk concat_append_dfio '1: a\n2: b' 'let lines=s.split("a\nb";"\n");let labels=mut.@();lp(let i=0;i<lines.len;i=i+1){labels=labels.append(s.fromint(i+1))};let tagged=mut.@();lp(let i=0;i<lines.len;i=i+1){tagged=tagged.append(s.concat(s.concat(labels.get(i);": ");lines.get(i)))};io.println(s.join("\n";tagged));'
mk interp_append_lit_get '1\n2' 'let labels=mut.@();lp(let i=0;i<2;i=i+1){labels=labels.append("\(i+1)")};lp(let j=0;j<2;j=j+1){io.println("\(labels.get(j))")};'
mk interp_append_seeded '1\n2' 'let labels=mut.@("");let l2=labels.pop();lp(let i=0;i<2;i=i+1){l2=l2.append("\(i+1)")};lp(let j=0;j<2;j=j+1){io.println("\(l2.get(j))")};'
# ── diagnostics added after the first batch (new-bug candidates) ───────────
mk bool_native_interp   '1'   'io.println("\(1==1)");'
mk bool_userfn_interp   '1'   'io.println("\(iseven(2))");'
mk keys_prop_len        '2'   'let m=@("a":1;"b":2);let k=m.keys;io.println("\(k.len)");'
mk keys_method_len      '2'   'let m=@("a":1;"b":2);let k=m.keys();io.println("\(k.len)");'
mk keys_method_direct   'a'   'let m=@("a":1;"b":2);let k=m.keys();io.println(k.get(0));'
mk split_method_direct  'b'   'let w="a,b,c".split(",");io.println(w.get(1));'
mk split_chain_direct   'b'   'io.println("a,b,c".split(",").get(1));'
mk trim_let_direct      'a'   'let x="  a  ";let y=x.trim();io.println(y);'
mk strlen_prop_direct   '6'   'let x="abcdef";io.println(x.len);'
mk get_const_let        '1'   "${A}let v=a.1;io.println(\"\\(v)\");"
mk get_const0_let       '3'   "${A}let v=a.0;io.println(\"\\(v)\");"
mk concat_append_interp_read '0: item\n1: item\n2: item' 'let items=mut.@();lp(let i=0;i<3;i=i+1){let line=s.concat(s.fromint(i);": item");items=items.append(line)};lp(let j=0;j<3;j=j+1){io.println("\(items.get(j))")};'
mk lit_append_interp_read 'x\nx\nx' 'let items=mut.@();lp(let i=0;i<3;i=i+1){items=items.append("x")};lp(let j=0;j<3;j=j+1){io.println("\(items.get(j))")};'
mk plusat_append_interp_read '0: item\n1: item\n2: item' 'let items=mut.@();lp(let i=0;i<3;i=i+1){items=items+@("\(i): item")};lp(let j=0;j<3;j=j+1){io.println("\(items.get(j))")};'
mk arr_contains_let     '1'   "${A}let r=a.contains(2);io.println(\"\\(r)\");"
# ── fold workaround (card canonical) as an oracle control ──────────────────
mk fold_loop_control '6' "${A}let acc=mut.0;lp(let i=0;i<a.len;i=i+1){acc=acc+a.get(i)};io.println(\"\\(acc)\");"
ls *.tk | wc -l
