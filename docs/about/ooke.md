---
title: ooke
slug: ooke
section: about
order: 8
---

A CMS and web framework built on the toke language. File-system routing, flat-file content, template engine with Markdown. Zero runtime dependencies. Ships as a single native binary.

- [GitHub](https://github.com/karwalski/ooke)
- [Ecosystem overview](/ecosystem)

## Built on toke's Web Server

ooke uses toke's `std.http` module under the hood for all HTTP serving, TLS, and request handling. Everything documented on the [web server page](/docs/about/web-server/) is available to ooke -- because ooke *is* a toke program.

If you need a full CMS with routing, templates, and content management, use ooke. If you need a raw API server or microservice with no framework overhead, use `std.http` directly. Both compile to the same kind of single native binary.

**Raw toke server:**

```toke
m=hello;
i=http:std.http;
f=home(req:http.$req):http.$res{ < http.ok("hello") };
f=main():i64{ http.get("/";home); < http.serve(8080 as u16) };
```

**ooke equivalent:**

```toke
m=page.home;
i=http:std.http;
i=tpl:ooke.template;
f=get(req:http.$req):http.$res{ < tpl.renderfile("templates/home.tkt";@()) };
```

ooke adds file-system routing, template rendering, and a content store on top of the same `std.http` primitives. Choose whichever fits your project.

## Quick start

```sh
ooke new mysite
cd mysite
ooke serve
ooke build
```

## What is ooke?

ooke is the web framework layer of the toke ecosystem. Write route handlers in toke, templates in `.tkt` files, and content in Markdown with YAML frontmatter. ooke compiles everything to a single native binary -- no Node.js, no Python, no nginx required.

## Features

### File-system routing

`pages/blog/[slug].tk` becomes `/blog/:slug`. Convention over configuration. Index files map to directory roots. Dynamic segments use `[name]` notation.

### Template engine

Templates use `{= expr =}` for expressions, `{! directive !}` for layout/blocks/partials. Layout inheritance, named blocks, partials, and a Markdown filter built in.

### Flat-file content store

Content lives as `.md` files with YAML frontmatter. `store.all("posts")`, `store.slug("posts"; "hello-world")`. No database. Version-controlled alongside code.

### Build and serve modes

`ooke build` renders static HTML with CSS inlining and minification. `ooke serve` runs a live HTTP server with an in-memory page cache. TLS supported.

### LLM-first scaffolding

toke's 55-char syntax means an LLM generates a complete ooke site in fewer tokens with higher correctness. `ooke gen page`, `ooke gen type`, `ooke gen api` scaffold common patterns instantly.

### Native binary output

ooke itself is a compiled binary linked against the toke stdlib. No interpreter, no VM, no GC. Runs on macOS, Linux, and anything clang can target.

## Template syntax

```
{! layout("base") !}
{! block("content") !}

<h1>{= page.title =}</h1>
<div class="body">
  {= body | md =}
</div>

{! end !}
```

## Route handler

```toke
m=page.blog.slug;
i=http:std.http;
i=tpl:ooke.template;
i=store:ooke.store;

f=get(req:http.$req):http.$res{
  let post=store.slug("posts";req.param("slug"));
  <tpl.renderfile("templates/blog/post.tkt";post);
};
```

## Custom Error Pages

ooke sites can define custom HTML templates for HTTP error responses. Place `.tkt` files in `templates/errors/` named by status code:

```
templates/
  errors/
    404.tkt    — Page Not Found
    405.tkt    — Method Not Allowed
    500.tkt    — Server Error
```

Each error template uses the same layout and syntax as any other ooke template. Use `{! layout("base") !}` to inherit the site layout so error pages match the rest of the site's styling. The template receives the current request context, so expressions like `{= request.path =}` and `{= request.method =}` are available to show the user what went wrong.

When ooke encounters an error, it checks `templates/errors/<code>.tkt`. If the template exists, it renders it through the full template engine. If no custom template is found, ooke falls back to a plain-text response with the status code and standard reason phrase.

This website is served by ooke. ooke is part of the [toke ecosystem](/ecosystem).
