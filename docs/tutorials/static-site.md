---
title: "Tutorial: Static Site with ooke"
slug: static-site
section: tutorials
order: 4
---

Build a personal blog with ooke: Markdown content, custom templates, static HTML output, and deploy with rsync. No database, no JavaScript framework, no runtime dependencies.

**Difficulty:** Beginner
**Time:** ~20 minutes
**Prerequisites:** toke compiler installed, basic familiarity with toke syntax

---

## 1. What we're building

A personal blog called "My toke Blog" with:

- A homepage that links to every post
- Individual blog post pages rendered from Markdown
- A shared layout with header, footer, and navigation
- CSS styling
- Static HTML output ready to deploy anywhere

Everything here is checked against ooke 3.x: the page code below is type-checked
against an ooke checkout by `make check-docs` in the toke repository, and the
project builds with `ooke build` as shown.

---

## 2. LLM prompts

Before building manually, here are prompts you can give an LLM to generate ooke project files. These show the LLM-first workflow -- generate, review, adjust.

### Prompt: project config

```
Generate an ooke.toml config for a blog called "My toke Blog".
The [site] section should set name, url "https://myblog.example.com"
and language "en"; the [build] section should write to "build/"
with minify enabled.
```

Expected output:

```toml
[site]
name = "My toke Blog"
url = "https://myblog.example.com"
language = "en"

[build]
output = "build/"
minify = true
```

### Prompt: blog post template

```
Generate an ooke blog post template (.tkt file) that declares the layout
"base" and fills its "title" and "content" blocks. It should show the
post title in an h1 and the post body rendered from Markdown with the
md filter, followed by a link back to the homepage. The only values it
can use are title, body, slug, site.name, site.url and site.language.
```

### Prompt: homepage template

```
Generate an ooke homepage template (.tkt file) that declares the layout
"base". It should display site.name, a short intro paragraph, and a list
of links to the blog posts at /blog/<slug>/.
```

The LLM will generate `.tkt` files using the syntax shown in Section 4. Review the output and adjust to match the templates we build manually below. ooke templates have no loops or conditionals, so reject output that uses `{% for %}` or `{% if %}`.

---

## 3. Step by step

This section walks through building the blog manually, without LLM assistance.

### 3.1 Scaffold the project

```sh
ooke new myblog
cd myblog
```

This creates the project directory with the default structure:

```
myblog/
  ooke.toml          # project configuration
  pages/             # routes (.tk files); pages/index.tk is /
  templates/         # .tkt templates; base.tkt and index.tkt
  content/           # Markdown content
  static/            # CSS, images, fonts, served at /static/
```

### 3.2 Configure the project

Open `ooke.toml` and set your site details in the `[site]` and `[build]` sections (leave the rest of the scaffolded file as it is):

```toml
[site]
name = "My toke Blog"
url = "https://myblog.example.com"
language = "en"

[build]
output = "build/"
minify = true
```

The `[site]` values are available in every template as `site.name`, `site.url` and `site.language`.

### 3.3 Create the base layout

Replace `templates/base.tkt`. This is the shared HTML shell that every page uses:

```
<!DOCTYPE html>
<html lang="{= site.language =}">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{! yield("title") !} -- {= site.name =}</title>
  <link rel="stylesheet" href="/static/style.css">
</head>
<body>
  <header>
    <nav>
      <a href="/" class="site-name">{= site.name =}</a>
    </nav>
  </header>

  <main>
    {! yield("content") !}
  </main>

  <footer>
    <p>Built with <a href="https://tokelang.dev">ooke</a></p>
  </footer>
</body>
</html>
```

`{= expr =}` prints a value. `{! yield("content") !}` is where a page's `content` block goes. A page template starts with `{! layout("base") !}` and fills the blocks with `{! block("content") !}...{! end !}`.

### 3.4 Add a blog post

Create the content directory and your first post:

```sh
mkdir -p content/blog
```

Create `content/blog/hello-world.md`:

```markdown
---
title: Hello World
slug: hello-world
date: 2026-04-18
---

Welcome to my blog! This is my first post, written in
Markdown and rendered by ooke.

## Why toke?

toke is designed for LLM code generation -- fewer tokens,
faster inference, lower cost.
```

Frontmatter between `---` fences defines metadata. A post's template sees its `title`, its `slug` (from the frontmatter, or the file name when there is none) and its Markdown `body`. Other keys, such as `date`, are kept with the post but are not passed to the template.

Add a second post so the homepage has more than one link: `content/blog/second-post.md` with its own `title` and `slug` and a Markdown body.

### 3.5 Create the blog post route

Create `pages/blog/[slug].tk`. The `[slug]` in the file name tells ooke this is a dynamic route -- `/blog/hello-world`, `/blog/second-post`, and so on.

```toke
m=pages.blog.slug;
```

That one line is the whole page. ooke routes by the page's path, not by code in it: for `pages/blog/[slug].tk` it serves `/blog/:slug`, takes the posts from `content/blog/` (the directory the page sits in), and renders `templates/blog/[slug].tkt` once per post. `ooke build` writes each result to `build/blog/<slug>/index.html`.

A page that should run code for each request -- a JSON endpoint, a form handler -- goes under `pages/api/` and defines `f=get(req:i64):i64` or `f=post(req:i64):i64`; `ooke compile` builds those into a serve binary. A static blog needs none.

### 3.6 Create the blog post template

Create `templates/blog/[slug].tkt`:

```
{! layout("base") !}
{! block("title") !}{= title =}{! end !}
{! block("content") !}
<article class="post">
  <h1>{= title =}</h1>

  <div class="post-body">
    {= body | md =}
  </div>

  <p class="post-nav"><a href="/">&larr; All posts</a></p>
</article>
{! end !}
```

`{= body | md =}` renders the Markdown body to HTML. `{= title =}` is HTML-escaped, as every value is unless a filter says otherwise.

### 3.7 Create the homepage route

`ooke new` already created `pages/index.tk`, the route for `/`:

```toke
m=pages.index;
```

It renders `templates/index.tkt`.

### 3.8 Create the homepage template

Replace `templates/index.tkt`:

```
{! layout("base") !}
{! block("title") !}Home{! end !}
{! block("content") !}
<h1>{= site.name =}</h1>
<p class="intro">Welcome to my blog. I write about toke, ooke, and building things.</p>

<ul class="post-list">
  <li><a href="/blog/second-post/">Second post</a></li>
  <li><a href="/blog/hello-world/">Hello World</a></li>
</ul>
{! end !}
```

ooke templates have no loops, and a static page such as `/` is rendered with the `site.*` values only, so the list of posts is written into the template. Add a line to it when you add a post.

### 3.9 Add CSS

Create `static/style.css`:

```css
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
  font-family: system-ui, -apple-system, sans-serif;
  line-height: 1.6; max-width: 42rem;
  margin: 0 auto; padding: 2rem 1rem; color: #222;
}
header { margin-bottom: 2rem; border-bottom: 1px solid #e0e0e0; padding-bottom: 1rem; }
.site-name { font-weight: 700; font-size: 1.25rem; text-decoration: none; color: #222; }
h1 { margin-bottom: 0.5rem; }
.intro { font-size: 1.1rem; color: #555; margin-bottom: 2rem; }
.post-list { list-style: none; }
.post-list li { margin-bottom: 0.75rem; }
.post-list a { text-decoration: none; color: #0055aa; }
.post-list a:hover { text-decoration: underline; }
.post-body { margin: 1.5rem 0 2rem; }
.post-body p { margin-bottom: 1rem; }
.post-nav { margin-top: 2rem; padding-top: 1rem; border-top: 1px solid #e0e0e0; }
.post-nav a { color: #0055aa; text-decoration: none; }
footer { border-top: 1px solid #e0e0e0; margin-top: 2rem; padding-top: 1rem; color: #999; font-size: 0.85rem; }
```

Files in `static/` are served at `/static/` and copied to `build/static/` by `ooke build`.

### 3.10 Build

```sh
ooke build
```

ooke renders every route through its template and writes static HTML to `build/`. Each page becomes a directory with an `index.html` inside, giving clean URLs (`/blog/hello-world/` instead of `/blog/hello-world.html`):

```
build/
  index.html
  blog/hello-world/index.html
  blog/second-post/index.html
  static/style.css
```

### 3.11 Preview locally

```sh
ooke serve
```

Open `http://localhost:3000` in your browser. You should see the homepage with its two links. Click one to see the full post. `ooke serve` renders the templates when it starts, so restart it after you change a template or a post.

### 3.12 Deploy

ooke builds plain static files. Deploy them anywhere that serves HTML. The simplest method is rsync:

```sh
rsync -avz --delete build/ user@yourserver:/var/www/myblog/
```

Other options: copy `build/` to any static hosting provider (Cloudflare Pages, Netlify, S3), serve it with nginx or caddy, or run `ooke serve` for the built-in HTTP server.

---

## 4. Template syntax reference

Quick reference of the ooke template features used in this tutorial.

### Expressions

Print a value into the output:

```
{= site.name =}
{= title =}
{= body | md =}
```

Values are escaped by default for where they sit in the HTML (text, attribute, URL, script or style). A name the page has no value for prints nothing.

### Layouts and blocks

In the layout, mark where each block goes:

```
<main>{! yield("content") !}</main>
```

In a page template, declare the layout and fill the blocks:

```
{! layout("base") !}
{! block("content") !}
  <p>This goes where the layout yields "content"</p>
{! end !}
```

### Partials

Include a reusable fragment from `templates/`:

```
{! partial("partials/footer") !}
```

A partial sees the same values as the template that includes it.

### Filters

Filters transform a value with the pipe syntax `{= value | filter =}`. One filter per expression:

| Filter   | Description                              | Example                     |
|----------|------------------------------------------|-----------------------------|
| `md`     | Render Markdown to HTML                  | `{= body \| md =}`          |
| `escape` | Escape for the context (the default)     | `{= title \| escape =}`     |
| `raw`    | No escaping -- only for HTML you produced yourself | `{= snippet \| raw =}` |
| `upper`  | Convert to uppercase                     | `{= title \| upper =}`      |
| `lower`  | Convert to lowercase                     | `{= title \| lower =}`      |
| `trim`   | Strip leading/trailing whitespace        | `{= title \| trim =}`       |

### Directives summary

| Syntax | Purpose |
|---|---|
| `{= expr =}` | Print a value |
| `{! layout("name") !}` | Render this template inside `templates/<name>.tkt` |
| `{! block("name") !}...{! end !}` | Fill a named block |
| `{! yield("name") !}` | In a layout: where a block goes |
| `{! partial("name") !}` | Include `templates/<name>.tkt` |
| `{# comment #}` | A comment; not rendered |

---

## 5. Troubleshooting

### The page renders but a block is empty

The layout must `{! yield("name") !}` every block a page fills, and each `{! block("name") !}` needs its own `{! end !}`. A stray `{! end !}` with no open block is an error.

### A layout or partial is not found

`{! layout("base") !}` reads `templates/base.tkt`; `{! partial("partials/footer") !}` reads `templates/partials/footer.tkt`. The name is relative to `templates/` and has no `.tkt` extension; `..`, a leading `/` and backslashes are refused.

### A blog post is missing from the build

- The `.md` file must be in `content/blog/` -- the directory named by the page's own directory under `pages/`
- The post needs a slug: from its frontmatter `slug`, or its file name
- Check the `ooke build` log: a post without a slug, or a template that fails to render, is reported there

### "route not found" for /blog/hello-world

- The file `pages/blog/[slug].tk` must exist, with the square brackets in the file name
- The slug in the URL must match a post's slug
- Run `ooke build` and look in `build/blog/` to see what was generated

### CSS not loading

- Static files must be in the `static/` directory
- Reference them as `/static/style.css`, not `style.css`
- After `ooke build`, confirm the file was copied to `build/static/style.css`

### Markdown not rendering as HTML

- Use the `md` filter: `{= body | md =}`
- Without it, the Markdown source is printed as escaped text

---

## 6. Exercises

Try extending the blog on your own. These are listed roughly by difficulty.

### 6.1 Add an About page

Add `pages/about.tk` (`m=pages.about;`) and `templates/about.tkt`, which declares the `base` layout like the homepage does. ooke serves it at `/about` and builds `build/about/index.html`. Link to it from the `<nav>` in `base.tkt`.

### 6.2 Move the footer into a partial

Move the `<footer>` element of `base.tkt` into `templates/partials/footer.tkt` and include it with `{! partial("partials/footer") !}`.
