# tkgrep

A CLI file search tool built in toke. Searches for text patterns in files,
similar to grep.

## Build

```bash
tkc --out tkgrep main.tk
```

## Usage

```
$ ./tkgrep [-icnv] <pattern> <file>
```

Example:

```
$ ./tkgrep main src/app.tk
12:f=main():$i64{
$ ./tkgrep -ci error log.txt
4
```

Flags: `-i` case-insensitive, `-c` count only, `-n` suppress line numbers,
`-v` invert (print non-matching lines).

## Features

- Substring pattern matching with line numbers
- Case-insensitive (`-i`), count (`-c`), no-line-numbers (`-n`), invert (`-v`)
- Exit codes for scripting (0 = match found, 1 = no match, 2 = usage/file error)

## Tutorial

See [CLI Tool Tutorial](/docs/tutorials/cli-tool/) for a step-by-step guide.
