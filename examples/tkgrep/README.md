# tkgrep

A CLI file search tool built in toke. Searches for text patterns in files,
similar to grep.

## Build

```bash
tkc --out tkgrep main.tk
```

## Usage

```
$ ./tkgrep --allow-read [-icnv] <pattern> <file>
```

toke programs run with no file access by default; `--allow-read` lets
tkgrep read the file. Without it the program stops with a `CAP001` message
naming the missing flag.

Example:

```
$ ./tkgrep --allow-read main src/app.tk
12:f=main():$i64{
$ ./tkgrep --allow-read -ci error log.txt
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
