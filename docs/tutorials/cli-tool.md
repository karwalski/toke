---
title: "Tutorial: CLI Tool (tkgrep)"
slug: cli-tool
section: tutorials
order: 5
---

Build a grep-like command-line search tool in toke. By the end you will have a program that searches files for text patterns, displays matching lines with line numbers, and supports flags for case-insensitive matching, match counting, line-number suppression, and inverted matches.

**Time:** ~20 minutes
**Difficulty:** Beginner
**Prerequisites:** toke compiler installed, basic familiarity with toke syntax

---

## 1. What We're Building

`tkgrep` is a single-file CLI tool that does four things:

- **Pattern matching** -- finds lines containing a substring in a given file.
- **Line-numbered output** -- prints each matching line prefixed with its line number.
- **CLI flags** -- supports `-i` (case-insensitive), `-c` (count only), `-n` (suppress line numbers), and `-v` (invert match).
- **Exit codes** -- returns 0 when matches are found, 1 for no matches, and 2 for errors, making it scriptable.

The entire project lives in a single file:

| File | Responsibility |
|------|---------------|
| `main.tk` | Argument parsing, file I/O, pattern matching, output formatting, entry point |

**Source code:** `toke/examples/tkgrep/`

---

## 2. LLM Prompts

You can recreate this project by feeding the following prompt to an LLM that knows toke syntax.

### Prompt -- tkgrep

> Generate a toke module `tkgrep.main` in a file called `main.tk`.
> Import `std.io` as `io`, `std.file` as `file`, `std.str` as `str`, and `std.args` as `args`.
>
> Define a sum type `$greperr` with variants `$usage:$str`, `$fileerr:$str`, `$nopattern:void`.
>
> Define a struct `$opts` with fields `pattern:$str`, `path:$str`, `ignorecase:bool`, `countonly:bool`, `nolinenum:bool`, `invert:bool`.
>
> Implement:
>
> - `printusage():void` -- print usage text showing `tkgrep <pattern> <file>` and describe each flag
> - `parseargs():$opts!$greperr` -- walk argv, collect flags starting with `-`, collect positional args into a mutable array with `@()`; return `$err` if fewer than 2 positionals
> - `matches(line:$str;pattern:$str;ignorecase:bool):bool` -- lowercase both strings when `ignorecase` is true, then check `str.contains`
> - `formatline(linenum:u64;line:$str;shownum:bool):$str` -- prepend `linenum:` when `shownum` is true using `str.buf()`, otherwise return the bare line
> - `search(opts:$opts):u64!$greperr` -- read the file with `file.read`, split into lines, loop with `lp` and print/count matches
> - `main():i64` -- parse args, run search, map errors to stderr messages and exit codes (0 = match, 1 = no match, 2 = error)
>
> Handle every fallible call with `mt expr { $ok:...; $err:... }` match syntax.
> Use `mut.` for mutable bindings and `lp` for loops.
> Use `<` as the return keyword. Use `;` to separate parameters in function calls.

---

## 3. Step by Step

### 3.1 Create the project

Create a directory and an empty source file:

```bash
mkdir -p tkgrep
touch tkgrep/main.tk
```

### 3.2 Module header and imports

Every toke file starts with a module declaration and its imports. We need standard I/O, file reading, string utilities, and command-line argument access.

```toke
m=tkgrep.main;
i=io:std.io;
i=file:std.file;
i=str:std.str;
i=args:std.args;
```

`m=` declares the module name. Each `i=` binds a standard library module to a short alias.

### 3.3 Define the error type and options struct

Before writing any logic, define the types that flow through the program. A sum type captures the three ways tkgrep can fail. A struct captures the parsed CLI options.

```toke
t=$greperr{$usage:$str;$fileerr:$str;$nopattern:void};

t=$opts{
  pattern:$str;
  path:$str;
  ignorecase:bool;
  countonly:bool;
  nolinenum:bool;
  invert:bool
};
```

Sum type variants start with `$`. The `$nopattern` variant carries no data, so it uses `void`. The `$opts` struct holds everything extracted from argv.

### 3.4 Usage text

A small helper prints the usage banner. It returns `$i64` (so it can end with a
`<0`); it is called on argument errors.

```toke
f=printusage():$i64{
  io.println("usage: tkgrep <pattern> <file>");
  io.println("");
  io.println("search for lines matching <pattern> in <file>");
  io.println("prints matching lines with line numbers.");
  io.println("");
  io.println("options:");
  io.println("  -i    case-insensitive matching");
  io.println("  -c    count matches only");
  io.println("  -n    suppress line numbers");
  io.println("  -v    invert match (print non-matching lines)");
  <0
};
```

### 3.5 Parse command-line arguments

This is the largest function. It walks argv, collects flags, and gathers
positional arguments into a mutable array. It fails with a `$greperr` when there
is no pattern (`$nopattern`) or no file path (`$usage`).

```toke
f=parseargs():$opts!$greperr{
  let ignorecase=mut.false;
  let countonly=mut.false;
  let nolinenum=mut.false;
  let invert=mut.false;
  let positional=mut.@();

  lp(let i=1;i<(args.count() as i64);i=i+1){
    let arg=args.get(i);
    if(str.startswith(arg;"-")){
      if(str.contains(arg;"i")){ignorecase=true};
      if(str.contains(arg;"c")){countonly=true};
      if(str.contains(arg;"n")){nolinenum=true};
      if(str.contains(arg;"v")){invert=true}
    };
    if(str.startswith(arg;"-")=false){
      positional=positional.push(arg)
    }
  };
  if(positional.len=0){
    <$greperr{$nopattern:0}
  };
  if(positional.len<2){
    <$greperr{$usage:"need <pattern> and <file>"}
  };
  <$opts{
    pattern:positional.get(0);
    path:positional.get(1);
    ignorecase:ignorecase;
    countonly:countonly;
    nolinenum:nolinenum;
    invert:invert
  }
};
```

Key patterns to notice:

- **`!$greperr`** after the return type means this function can fail with a `$greperr`.
- The **ok return** is just the bare value (`<$opts{...}`); the **error return**
  constructs an error variant (`<$greperr{$usage:"…"}`). There is no `$ok(...)` /
  `$err(...)` wrapper — the `T!$err` signature already says which is which.
- **`mut.@()`** creates a mutable empty array; **`=`** is equality
  (`startswith(arg;"-")=false`).

### 3.6 Pattern matching

A small pure function checks whether a line contains the search pattern. When
case-insensitive mode is on, both strings are lowered first.

```toke
f=matches(line:$str;pattern:$str;ignorecase:bool):bool{
  if(ignorecase){
    <str.contains(str.lower(line);str.lower(pattern))
  };
  <str.contains(line;pattern)
};
```

### 3.7 Output formatting

Each matching line can optionally be prefixed with its line number, built with a
string buffer.

```toke
f=formatline(linenum:u64;line:$str;shownum:bool):$str{
  if(shownum){
    let buf=str.buf();
    str.add(buf;str.fromint(linenum as i64));
    str.add(buf;":");
    str.add(buf;line);
    <str.done(buf)
  };
  <line
};
```

### 3.8 The search function

The core loop reads a file, splits it into lines, and checks each line. It fails
with `$fileerr` if the file cannot be read, and otherwise returns the match
**count** — `0` is a perfectly valid count (the error union no longer conflates
it with failure; see toke story 114.55).

```toke
f=search(opts:$opts):u64!$greperr{
  let content=mt file.read(opts.path) {
    $ok:v v;
    $err:e <$greperr{$fileerr:str.concat("cannot read file: ";opts.path)}
  };
  let lines=str.split(content;"\n");
  let count=mut.0;
  let shownum=opts.nolinenum=false;

  lp(let i=0;i<(lines.len as i64);i=i+1){
    let line=lines.get(i);
    let hit=matches(line;opts.pattern;opts.ignorecase);
    let show=mut.hit;
    if(opts.invert){
      show=hit=false
    };
    if(show){
      count=count+1;
      if(opts.countonly=false){
        let linenum=(i+1) as u64;
        io.println(formatline(linenum;line;shownum))
      }
    }
  };
  if(opts.countonly){
    io.println(str.fromint(count as i64))
  };
  <count as u64
};
```

- **`mt file.read(opts.path) {…}`** — the `mt` keyword matches on a result.
  `$ok:v v` unwraps the success value; `$err:e <$greperr{$fileerr:…}` early-returns
  the file error.
- **`<count as u64`** is the ok return. Because `search` is a `T!$err` function,
  a `count` of `0` (no matches) is still `$ok` — the caller decides that "no
  matches" maps to exit code 1, not an error.

### 3.9 Reporting errors and the entry point

`reporterr` dispatches a `$greperr` to a stderr message (and the usage banner for
argument errors), returning exit code 2:

```toke
f=reporterr(e:$greperr;showusage:bool):$i64{
  mt e {
    $usage:msg io.eprintln(str.concat("tkgrep: ";msg));
    $fileerr:msg io.eprintln(str.concat("tkgrep: ";msg));
    $nopattern:z io.eprintln("tkgrep: need <pattern> and <file>")
  };
  if(showusage){
    printusage()
  };
  <2
};
```

`main` ties it together — parse, search, and map outcomes to exit codes. Each
`$err` arm early-returns `reporterr(e;…)`:

```toke
f=main():$i64{
  let opts=mt parseargs() {
    $ok:o o;
    $err:e <reporterr(e;true)
  };
  let count=mt search(opts) {
    $ok:c c;
    $err:e <reporterr(e;false)
  };
  if(count=0){
    <1
  };
  <0
};
```

Exit codes follow Unix convention: 0 for success (matches found), 1 for "no
match" (not an error), and 2 for actual errors. The inner `mt e {…}` in
`reporterr` dispatches each `$greperr` variant to a message on stderr.
---

## 4. Build and Run

### 4.1 Compile

```bash
tkc main.tk -o tkgrep
```

This produces a `tkgrep` binary in the current directory.

### 4.2 Create a test file

```bash
cat > sample.txt << 'EOF'
the quick brown fox
jumps over the lazy dog
the fox runs fast
a cat sleeps quietly
the dog barks loudly
EOF
```

### 4.3 Basic search

```bash
$ ./tkgrep fox sample.txt
1:the quick brown fox
3:the fox runs fast
```

Lines containing "fox" are printed with their 1-based line numbers.

### 4.4 Case-insensitive search

```bash
$ ./tkgrep -i FOX sample.txt
1:the quick brown fox
3:the fox runs fast
```

The `-i` flag lowercases both the pattern and each line before comparing.

### 4.5 Count matches only

```bash
$ ./tkgrep -c the sample.txt
4
```

With `-c`, only the count of matching lines is printed.

### 4.6 Suppress line numbers

```bash
$ ./tkgrep -n fox sample.txt
the quick brown fox
the fox runs fast
```

The `-n` flag removes the `linenum:` prefix from each line.

### 4.7 Invert match

```bash
$ ./tkgrep -v fox sample.txt
2:jumps over the lazy dog
4:a cat sleeps quietly
5:the dog barks loudly
6:
```

With `-v`, lines that do not match the pattern are printed instead.

### 4.8 Combined flags

Flags can be combined in a single argument:

```bash
$ ./tkgrep -ic THE sample.txt
4
```

This counts matches case-insensitively. The flag parser checks each character individually, so `-ic` enables both `-i` and `-c`.

### 4.9 Exit codes

```bash
$ ./tkgrep fox sample.txt; echo "exit: $?"
1:the quick brown fox
3:the fox runs fast
exit: 0

$ ./tkgrep zebra sample.txt; echo "exit: $?"
exit: 1

$ ./tkgrep fox missing.txt; echo "exit: $?"
tkgrep: cannot read file: missing.txt
exit: 2
```

Exit codes make `tkgrep` usable in shell scripts and pipelines:

```bash
./tkgrep "todo" main.tk && echo "found todos"
```

---

## 5. Troubleshooting

### "too few arguments"

You must pass both a pattern and a file path. If your pattern contains spaces, wrap it in quotes:

```bash
# Wrong -- missing file path
./tkgrep "pattern"

# Right
./tkgrep "brown fox" sample.txt
```

### "cannot read file: somefile.txt"

The file does not exist or is not readable. Check the path with `ls -la somefile.txt`. Paths are relative to the current working directory, not the location of the `.tk` source files. `tkgrep` does not expand `~` or glob patterns -- pass the full path.

### No output, exit code 1

The pattern was not found. Double-check spelling. Try `-i` in case the casing differs.

### Pattern matches too broadly

`tkgrep` does substring matching, not regular expressions. The pattern `"a"` matches every line containing the letter `a`. Use a longer, more specific pattern:

```bash
# Too broad
./tkgrep "a" main.tk

# More specific
./tkgrep "args" main.tk
```

### Binary files produce garbage output

`tkgrep` reads files as UTF-8 text and splits on `\n`. Binary files will produce unpredictable output or no matches. Stick to plain text files.

### Large files use significant memory

The entire file is read into memory with `file.read` and then split into an array. For very large files (hundreds of megabytes) this may use significant memory. A streaming line reader would be more efficient but is beyond the scope of this tutorial.

### Flags after positional arguments are ignored

The parser treats any argument not starting with `-` as a positional. Place flags before the pattern:

```bash
# Right -- flags before pattern
./tkgrep -i "pattern" file.txt

# Wrong -- flag after positionals, treated as a third positional
./tkgrep "pattern" file.txt -i
```

---

## 6. Exercises

Try extending `tkgrep` with the following features. Each exercise builds on the base implementation.

### Exercise 1 -- Regex support

Replace the `str.contains` check in `matches` with a regex engine call. Add a `-r` flag that switches from substring to regex matching. The `std.re` module provides `re.match(pattern;text):bool`.

**Hint:** add a `regex:bool` field to `$opts`, check it in `parseargs`, and branch inside `matches`:

```toke
if(opts.regex){
  <re.match(pattern;line)
};
```

### Exercise 2 -- Recursive directory search

Accept a directory path instead of a single file. Walk the directory tree with `file.listdir` and search each file. Print results prefixed with the file path:

```
src/main.tk:12:f=main():i64{
src/util.tk:3:f=helper(x:i64):i64{
```

**Hint:** write a `walk(dir:$str;opts:$opts):u64!$greperr` function that loops over directory entries and calls `search` on each regular file. You will need to update `formatline` to accept an optional file path prefix.

### Exercise 3 -- Coloured output

When stdout is a terminal, highlight the matching portion of each line using ANSI escape codes:

```toke
let red="\x1b[31m";
let reset="\x1b[0m";
```

Split the line around the matched substring and insert colour codes around it. Consider what happens when the pattern appears multiple times on one line.

### Exercise 4 -- Context lines

Add `-A <n>` (after) and `-B <n>` (before) flags to show surrounding lines, separated by `--` between groups. This requires buffering previous lines in a ring buffer and looking ahead after each match.

**Hint:** store the last `n` lines in a mutable array. When a match is found, print the buffered lines first, then print the next `n` lines after the match. Use a `--` separator between non-contiguous groups.

### Exercise 5 -- Multiple file support

Accept more than one file on the command line. Collect all positional arguments after the pattern into an array of paths, then loop over them. Print a filename prefix on each output line. Track a combined exit code: 0 if any file had matches, 1 if none did, 2 on any error.
