# tkgrep

A CLI file search tool built in toke. Searches for text patterns in files,
similar to grep.

## Build

```bash
tkc main.tk -o tkgrep
```

## Usage

```
$ ./tkgrep <pattern> <file...>
```

Example:

```
$ ./tkgrep "function" src/main.tk src/utils.tk
src/main.tk:12: f=main():i64{
src/utils.tk:3: f=helper(x:i64):i64{
```

## Features

- Pattern matching across multiple files
- File and line number display
- Exit codes for scripting (0 = match found, 1 = no match)

## Tutorial

See [CLI Tool Tutorial](/docs/tutorials/cli-tool/) for a step-by-step guide.
