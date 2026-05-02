---
title: std.file
slug: file
section: reference/stdlib
order: 15
---

**Status: Implemented** -- C runtime backing.

The `std.file` module provides functions for reading, writing, and managing files and directories on the local file system. All paths are UTF-8 strings. Operations that can fail return a result type with `$fileerr`.

> **Implemented functions (from `file.tki`):** `file.read`, `file.write`, `file.append`, `file.exists`, `file.delete`, `file.list`, `file.isdir`, `file.mkdir`, `file.copy`, `file.listall`. Functions documented in earlier versions (`mkdir_p`, `rmdir`, `rmdir_r`, `is_file`, `move`, `size`, `mtime`, `join`, `basename`, `dirname`, `absolute`, `ext`, `readlines`, `glob`) are not in the current tki.

## Types

### $fileerr

A sum type representing file operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $notfound | $str | The file or directory does not exist |
| $permission | $str | The process lacks permission to perform the operation |
| $io | $str | A general I/O error occurred (disk full, interrupted, etc.) |

## Functions

### file.read(path: $str): $str!$fileerr

Reads the entire contents of the file at `path` and returns it as a UTF-8 string. Returns `$fileerr.$notfound` if the file does not exist, `$fileerr.$permission` if access is denied, or `$fileerr.$io` on other I/O failures.

```toke
m=example;
i=file:std.file;

f=readfile():$str{
  let r=file.read("/tmp/data.txt");
  r|{
    $ok:s  s;
    $err:e ""
  }
};
```

### file.write(path: $str; content: $str): bool!$fileerr

Writes `content` to the file at `path`, creating the file if it does not exist and truncating it if it does. Returns `true` on success.

```toke
m=example;
i=file:std.file;

f=writefile():i64{
  file.write("/tmp/data.txt";"hello world")|{
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.append(path: $str; content: $str): bool!$fileerr

Appends `content` to the end of the file at `path`, creating the file if it does not exist. Returns `true` on success; does not truncate existing content.

```toke
m=example;
i=file:std.file;

f=appendfile():i64{
  file.append("/tmp/log.txt";"new line\n")|{
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.exists(path: $str): bool

Returns `true` if a file or directory exists at `path`, `false` otherwise. This function is infallible.

```toke
m=example;
i=file:std.file;

f=checkfile():bool{
  let y=file.exists("/tmp/data.txt");
  < y
}
```

### file.delete(path: $str): bool!$fileerr

Deletes the file at `path`. Returns `true` on success. Returns `$fileerr.$notfound` if the file does not exist, or `$fileerr.$permission` if access is denied.

```toke
m=example;
i=file:std.file;

f=delfile():i64{
  file.delete("/tmp/temp.txt")|{
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.list(dir: $str): @($str)!$fileerr

Returns an array of entry names (files and subdirectories, not full paths) in the directory `dir`. Returns `$fileerr.$notfound` if the directory does not exist, or `$fileerr.$permission` if access is denied.

```toke
m=example;
i=file:std.file;

f=listdir():i64{
  file.list("/tmp")|{
    $ok:entries 0;
    $err:e      1
  }
};
```

### file.listall(dir: $str): @($str)!$fileerr

Returns all file paths under `dir` recursively as full paths. Returns `$fileerr.$notfound` if the directory does not exist.

```toke
m=example;
i=file:std.file;

f=listrecursive():i64{
  file.listall("/tmp")|{
    $ok:entries 0;
    $err:e      1
  }
};
```

### file.isdir(path: $str): bool

Returns `true` if `path` refers to a directory, `false` for any other entry type or if the path does not exist. This function is infallible.

```toke
m=example;
i=file:std.file;

f=checkdir():bool{
  let d=file.isdir("/tmp");
  < d
}
```

### file.mkdir(path: $str): bool!$fileerr

Creates a single directory at `path`. Returns `$fileerr.$io` if the parent directory does not exist.

```toke
m=example;
i=file:std.file;

f=makedir():i64{
  file.mkdir("/tmp/mydir")|{
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.copy(src: $str; dst: $str): bool!$fileerr

Copies the file at `src` to `dst`, creating `dst` if it does not exist and overwriting it if it does. Returns `$fileerr.$notfound` if `src` does not exist.

```toke
m=example;
i=file:std.file;

f=copyfile():i64{
  file.copy("/tmp/original.txt";"/tmp/backup.txt")|{
    $ok:ok 0;
    $err:e 1
  }
};
```

## Usage Examples

List a directory, filter to `.tk` files, read each one, count lines, and print a summary:

```toke
m=tksummary;
i=file:std.file;
i=str:std.str;
i=log:std.log;

f=countlines(content:$str):i64{
  let lines=str.split(content;"\n");
  < lines.len
};

f=summarize(dir:$str):i64{
  let entries=file.list(dir)|{
    $ok:e  e;
    $err:e @()
  };

  let total=mut.0;
  lp(let i=0;i<entries.len;i=i+1){
    let name=entries.get(i)|{$ok:v v;$err:e ""};
    let path=str.concat(dir;str.concat("/";name));
    let r=file.read(path);
    let n=r|{$ok:content countlines(content);$err:e 0};
    total=total+n;
    log.info(name;@())
  };
  < 0
};
```

## See Also

- [std.str](/docs/stdlib/str) -- string manipulation for processing file content
- [std.json](/docs/stdlib/json) -- parse JSON files after reading them with `file.read`
- [std.log](/docs/stdlib/log) -- log file paths and operation outcomes
