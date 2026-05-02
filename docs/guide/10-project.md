---
title: Lesson 10 — Build a Complete Project
slug: 10-project
section: learn
order: 10
---

**Estimated time: ~30 minutes**

In this final lesson, you will build a complete toke application: a CLI bookmark manager called `bm`. It stores URLs with tags, persists data to a JSON file, and supports add, list, search, and delete operations.

This lesson ties together modules, functions, types, error handling, collections, string operations, file I/O, and JSON -- everything from the previous nine lessons.

## Project design

### Features

- **Add** a bookmark with a URL and tags
- **List** all bookmarks
- **Search** bookmarks by tag or URL substring
- **Delete** a bookmark by ID
- **Persist** data to a JSON file between runs

### Module structure

```
bm/
  main.tk       m=bm.main       -- CLI entry point and argument parsing
  store.tk      m=bm.store      -- Bookmark storage, load/save, CRUD operations
  model.tk      m=bm.model      -- Type definitions
```

Three files, each with a clear responsibility.

## Step 1: Define the model

The model module defines the core types:

`bm/model.tk`:

```
m=bm.model;

t=$bookmark{
  id:u64;
  url:$str;
  title:$str;
  tags:@$str
};

t=$bookmarkdb{
  bookmarks:@$bookmark;
  nextid:u64
};

t=$bmerr{
  $fileerr:$str;
  $parseerr:$str;
  $notfound:u64
};
```

`$bookmark` is a single entry. `$bookmarkdb` holds the array of bookmarks and a counter for generating unique IDs. `$bmerr` covers all failure modes.

The interface file for this module would be:

`bm/model.tki`:

```
m=bm.model;

t=$bookmark{id:u64;url:$str;title:$str;tags:@$str};
t=$bookmarkdb{bookmarks:@$bookmark;nextid:u64};
t=$bmerr{$fileerr:$str;$parseerr:$str;$notfound:u64};
```

## Step 2: Build the store

The store module handles persistence and CRUD operations:

`bm/store.tk`:

```text
m=bm.store;
i=file:std.file;
i=json:std.json;
i=str:std.str;
i=m:bm.model;

f=dbpath():$str{
  <"bookmarks.json";
};

f=load():m.$bookmarkdb!m.$bmerr{
  if(!file.exists(dbpath())){
    <m.$bookmarkdb{bookmarks:@();nextid:1};
  };
  let content=file.read(dbpath())!m.$bmerr;
  let db=json.dec(content)!m.$bmerr;
  <db;
};

f=save(db:m.$bookmarkdb):void!m.$bmerr{
  let data=json.pretty(db);
  file.write(dbpath();data)!m.$bmerr;
};

f=add(db:m.$bookmarkdb;url:$str;title:$str;tags:@$str):m.$bookmarkdb{
  let bm=m.$bookmark{
    id:db.nextid;
    url:url;
    title:title;
    tags:tags
  };
  let newbookmarks=db.bookmarks.push(bm);
  <m.$bookmarkdb{
    bookmarks:newbookmarks;
    nextid:db.nextid+1
  };
};

f=delete(db:m.$bookmarkdb;id:u64):m.$bookmarkdb!m.$bmerr{
  let found=mut.false;
  let result=mut.@();
  lp(let i=0;i<db.bookmarks.len;i=i+1){
    if(db.bookmarks.get(i).id=id){
      found=true;
    }el{
      result=result.push(db.bookmarks.get(i));
    };
  };
  if(!found){
    <m.$bmerr{$notfound:id};
  };
  <m.$bookmarkdb{
    bookmarks:result;
    nextid:db.nextid
  };
};

f=search(db:m.$bookmarkdb;query:$str):@m.$bookmark{
  let q=str.lower(query);
  let result=mut.@();
  lp(let i=0;i<db.bookmarks.len;i=i+1){
    let bm=db.bookmarks.get(i);
    let matched=mut.false;
    if(str.contains(str.lower(bm.url);q)){
      matched=true;
    };
    if(str.contains(str.lower(bm.title);q)){
      matched=true;
    };
    lp(let j=0;j<bm.tags.len;j=j+1){
      if(str.contains(str.lower(bm.tags.get(j));q)){
        matched=true;
      };
    };
    if(matched){
      result=result.push(bm);
    };
  };
  <result;
};
```

Let's walk through the key functions:

- **`load`** checks if the file exists. If not, it returns an empty database with `nextid` starting at 1. Otherwise, it reads and parses the JSON file.
- **`save`** encodes the database as pretty-printed JSON and writes it to disk.
- **`add`** creates a new `$bookmark` with the next available ID, appends it to the array, and increments the counter. Note that this returns a new `$bookmarkdb` -- data structures are not mutated in place.
- **`delete`** iterates through bookmarks, skipping the one to delete. If not found, it returns a `$notfound` error.
- **`search`** does a case-insensitive search across URL, title, and tags.

## Step 3: Wire up the CLI

The main module handles argument parsing and dispatches to the store:

`bm/main.tk`:

```text
m=bm.main;
i=io:std.io;
i=str:std.str;
i=store:bm.store;
i=m:bm.model;

f=printbookmark(bm:m.$bookmark):void{
  let tagsstr=str.join(bm.tags;", ");
  io.println("  [\(bm.id as $str)] \(bm.title)");
  io.println("      \(bm.url)");
  if(bm.tags.len>0){
    io.println("      tags: \(tagsstr)");
  };
};

f=printbookmarks(bms:@m.$bookmark):void{
  if(bms.len=0){
    io.println("  (no bookmarks)");
  }el{
    lp(let i=0;i<bms.len;i=i+1){
      printbookmark(bms.get(i));
    };
  };
};

f=cmdadd(url:$str;title:$str;tagstr:$str):void{
  mt store.load() {
    $ok:db  {
      let tags=str.split(tagstr;",");
      let cleantags=mut.@();
      lp(let i=0;i<tags.len;i=i+1){
        let t=str.trim(tags.get(i));
        if(str.len(t)>0){
          cleantags=cleantags.push(t);
        };
      };
      let updated=store.add(db;url;title;cleantags);
      mt store.save(updated) {
        $ok:v   io.println("Added bookmark");
        $err:e  io.println("Error saving")
      };
    };
    $err:e  io.println("Error loading database: \(e as $str)");
  };
};

f=cmdlist():void{
  mt store.load() {
    $ok:db  {
      io.println("Bookmarks (\(db.bookmarks.len as $str) total):");
      printbookmarks(db.bookmarks);
    };
    $err:e  io.println("Error: \(e as $str)");
  };
};

f=cmdsearch(query:$str):void{
  mt store.load() {
    $ok:db  {
      let results=store.search(db;query);
      io.println("Search results for \"\(query)\" (\(results.len as $str) found):");
      printbookmarks(results);
    };
    $err:e  io.println("Error: \(e as $str)");
  };
};

f=cmddelete(idstr:$str):void{
  mt store.load() {
    $ok:db  {
      let id=str.toint(idstr) as u64;
      mt store.delete(db;id) {
        $ok:updated  {
          mt store.save(updated) {
            $ok:v   io.println("Deleted bookmark");
            $err:e  io.println("Error saving")
          };
        };
        $err:e  mt e {
          $notfound:n   io.println("Bookmark not found");
          $fileerr:msg  io.println("File error");
          $parseerr:msg io.println("Parse error")
        }
      };
    };
    $err:e  io.println("Error: \(e as $str)");
  };
};

f=usage():void{
  io.println("bm - bookmark manager");
  io.println("");
  io.println("Usage:");
  io.println("  bm add <url> <title> [tags]    Add a bookmark (tags: comma-separated)");
  io.println("  bm list                        List all bookmarks");
  io.println("  bm search <query>              Search bookmarks");
  io.println("  bm delete <id>                 Delete a bookmark by ID");
};

f=main():i64{
  io.println("bm - Interactive bookmark manager");
  io.println("Commands: add, list, search, delete, quit");
  io.println("");

  lp(let run=mut.true;run;run=run){
    io.print("bm> ");
    let input=str.trim(io.readline());

    if(str.startswith(input;"add ")){
      let rest=str.slice(input;4;str.len(input)-4);
      let parts=str.split(rest;" ");
      if(parts.len<2){
        io.println("Usage: add <url> <title> [tags]");
      }el{
        let url=parts.get(0);
        let title=parts.get(1);
        let tags=mut."";
        if(parts.len>2){
          tags=parts.get(2);
        };
        cmdadd(url;title;tags);
      };
    }el{
      if(input="list"){
        cmdlist();
      }el{
        if(str.startswith(input;"search ")){
          let query=str.slice(input;7;str.len(input)-7);
          cmdsearch(query);
        }el{
          if(str.startswith(input;"delete ")){
            let idstr=str.slice(input;7;str.len(input)-7);
            cmddelete(idstr);
          }el{
            if(input="quit"){
              run=false;
            }el{
              if(str.len(input)>0){
                io.println("Unknown command. Try: add, list, search, delete, quit");
              };
            };
          };
        };
      };
    };
  };

  io.println("Goodbye!");
  <0;
};
```

## Step 4: Build and run

```bash
tkc bm/model.tk bm/store.tk bm/main.tk -o bm
./bm
```

Example session:

```
bm - Interactive bookmark manager
Commands: add, list, search, delete, quit

bm> add https://toke.dev toke-website language,docs
Added bookmark [1]: toke-website
bm> add https://github.com/karwalski/toke github code,repos
Added bookmark [2]: github
bm> list
Bookmarks (2 total):
  [1] toke-website
      https://toke.dev
      tags: language, docs
  [2] github
      https://github.com/karwalski/toke
      tags: code, repos
bm> search toke
Search results for "toke" (1 found):
  [1] toke-website
      https://toke.dev
      tags: language, docs
bm> delete 1
Deleted bookmark [1]
bm> list
Bookmarks (1 total):
  [2] github
      https://github.com/karwalski/toke
      tags: code, repos
bm> quit
Goodbye!
```