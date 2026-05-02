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

- A homepage listing all posts by date (newest first)
- Individual blog post pages rendered from Markdown
- A shared layout with header, footer, and navigation
- CSS styling
- Static HTML output ready to deploy anywhere

---

## 2. LLM prompts

Before building manually, here are prompts you can give an LLM to generate ooke project files. These show the LLM-first workflow -- generate, review, adjust.

### Prompt: project config

```
Generate an ooke.toml config for a blog called "My toke Blog".
The site should have a title, description, base URL of "https://myblog.example.com",
and build output to the "out" directory. Include a [content] section
pointing to content/ and a [build] section with minify enabled.
```

Expected output:

```toml
[site]
title = "My toke Blog"
description = "A personal blog built with ooke"
base_url = "https://myblog.example.com"

[content]
dir = "content"

[build]
output = "out"
minify = true
```

### Prompt: blog post template

```
Generate an ooke blog post template (.tkt file) with layout inheritance
from "base". It should display the post title in an h1, the date,
the post body rendered from Markdown, and previous/next navigation links.
```

### Prompt: homepage template

```
Generate an ooke homepage template (.tkt file) that inherits from "base".
It should display the site title, a short intro paragraph, and list all
blog posts sorted by date (newest first). Each post entry shows the title
as a link and the date.
```

The LLM will generate `.tkt` files using the syntax shown in Section 4. Review the output and adjust to match the templates we build manually below.

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
  content/            # Markdown content files
  pages/              # route handlers (toke files)
  templates/          # .tkt template files
  static/             # CSS, images, fonts
```

### 3.2 Configure the project

Open `ooke.toml` and set your site details:

```toml
[site]
title = "My toke Blog"
description = "A personal blog built with ooke"
base_url = "https://myblog.example.com"

[content]
dir = "content"

[build]
output = "out"
minify = true
```

The `[site]` values are available in templates as `site.title`, `site.description`, and so on.

### 3.3 Create the base layout

Create `templates/base.tkt`. This is the shared HTML shell that every page inherits from:

```
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{= page.title =} -- {= site.title =}</title>
  <link rel="stylesheet" href="/style.css">
</head>
<body>
  <header>
    <nav>
      <a href="/" class="site-name">{= site.title =}</a>
    </nav>
  </header>

  <main>
    {! block("content") !}
    {! end !}
  </main>

  <footer>
    <p>Built with <a href="https://tokelang.dev">ooke</a></p>
  </footer>
</body>
</html>
```

`{= expr =}` prints values. `{! block("content") !}...{! end !}` defines a named block that child templates fill. `{! layout("base") !}` in a child template wraps it in this layout.

### 3.4 Add a blog post

Create the content directory and your first post:

```sh
mkdir -p content/posts
```

Create `content/posts/hello-world.md`:

```markdown
---
title: Hello World
slug: hello-world
date: 2026-04-18
summary: My first post on my new toke blog.
---

Welcome to my blog! This is my first post, written in
Markdown and rendered by ooke.

## Why toke?

toke is designed for LLM code generation -- fewer tokens,
faster inference, lower cost.
```

Frontmatter between `---` fences defines metadata. ooke loads content via `store.all("posts")` and `store.slug("posts"; slug)`.

Add a second post so the listing page has more than one entry. Create `content/posts/second-post.md` with similar frontmatter (`title`, `slug`, `date`, `summary`) and Markdown body.

### 3.5 Create the blog post route handler

Create `pages/blog/[slug].tk`. The `[slug]` in the filename tells ooke this is a dynamic route -- `/blog/hello-world`, `/blog/template-tricks`, etc.

```toke
m=page.blog.slug;
i=http:std.http;
i=tpl:ooke.template;
i=store:ooke.store;

f=get(req:http.$req):http.$res{
  let post=store.slug("posts";req.param("slug"));
  <tpl.renderfile("templates/posts/post.tkt";post);
};
```

The module declaration mirrors the URL path. `store.slug("posts"; req.param("slug"))` loads the post matching the URL slug, and `tpl.renderfile(...)` renders the template with that data.

### 3.6 Create the blog post template

Create `templates/posts/post.tkt`:

```
{! layout("base") !}
{! block("content") !}

<article class="post">
  <h1>{= post.title =}</h1>
  <time datetime="{= post.date =}">{= post.date =}</time>

  <div class="post-body">
    {= post.body | md =}
  </div>

  <nav class="post-nav">
    {% if prev %}
      <a href="/blog/{= prev.slug =}">&larr; {= prev.title =}</a>
    {% endif %}
    {% if next %}
      <a href="/blog/{= next.slug =}">{= next.title =} &rarr;</a>
    {% endif %}
  </nav>
</article>

{! end !}
```

### 3.7 Create the homepage route handler

Create `pages/index.tk`:

```toke
m=page.index;
i=http:std.http;
i=tpl:ooke.template;
i=store:ooke.store;

f=get(req:http.$req):http.$res{
  let posts=store.all("posts";@(by:"date";order:"desc"));
  <tpl.renderfile("templates/home.tkt";@(posts:posts));
};
```

### 3.8 Create the homepage template

Create `templates/home.tkt`:

```
{! layout("base") !}
{! block("content") !}

<h1>{= site.title =}</h1>
<p class="intro">Welcome to my blog. I write about toke, ooke, and building things.</p>

<section class="post-list">
  {% for post in posts %}
    <article class="post-summary">
      <time datetime="{= post.date =}">{= post.date =}</time>
      <h2><a href="/blog/{= post.slug =}">{= post.title =}</a></h2>
      <p>{= post.summary =}</p>
    </article>
  {% endfor %}
</section>

{! end !}
```

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
time { display: block; color: #666; font-size: 0.9rem; margin-bottom: 1rem; }
.intro { font-size: 1.1rem; color: #555; margin-bottom: 2rem; }
.post-list .post-summary { margin-bottom: 1.5rem; }
.post-list h2 a { text-decoration: none; color: #0055aa; }
.post-list h2 a:hover { text-decoration: underline; }
.post-body { margin: 1.5rem 0 2rem; }
.post-body p { margin-bottom: 1rem; }
.post-nav { display: flex; justify-content: space-between;
  margin-top: 2rem; padding-top: 1rem; border-top: 1px solid #e0e0e0; }
.post-nav a { color: #0055aa; text-decoration: none; }
footer { border-top: 1px solid #e0e0e0; padding-top: 1rem; color: #999; font-size: 0.85rem; }
```

Files in `static/` are copied as-is to the output directory during build.

### 3.10 Build

```sh
ooke build
```

ooke loads content, renders every route through its template, and writes static HTML to `out/`. Each page becomes a directory with an `index.html` inside, giving clean URLs (`/blog/hello-world/` instead of `/blog/hello-world.html`).

### 3.11 Preview locally

```sh
ooke serve
```

Open `http://localhost:3000` in your browser. You should see the homepage with posts listed newest first. Click a title to see the full post. `ooke serve` watches for changes and rebuilds automatically.

### 3.12 Deploy

ooke builds plain static files. Deploy them anywhere that serves HTML. The simplest method is rsync:

```sh
rsync -avz --delete out/ user@yourserver:/var/www/myblog/
```

Other options: copy `out/` to any static hosting provider (Cloudflare Pages, Netlify, S3), serve with nginx or caddy, or use `ooke serve` in production for the built-in HTTP server with caching.

---

## 4. Template syntax reference

Quick reference of ooke template features used in this tutorial and beyond.

### Expressions

Print a value into the output:

```
{= site.title =}
{= post.date =}
{= post.body | md =}
```

Expressions are HTML-escaped by default. Use `| raw` for unescaped output.

### Conditionals

```
{% if post.summary %}
  <p>{= post.summary =}</p>
{% else %}
  <p>No summary available.</p>
{% endif %}
```

### Loops

```
{% for post in posts %}
  <h2>{= post.title =}</h2>
{% endfor %}
```

### Layout inheritance

In the base template, define blocks:

```
{! block("content") !}
  <p>Default content if not overridden</p>
{! end !}
```

In a child template, declare the layout and fill the block:

```
{! layout("base") !}
{! block("content") !}
  <p>This replaces the default content</p>
{! end !}
```

### Partials

Include a reusable template fragment:

```
{! partial("components/footer") !}
```

Partials receive the current template context. Pass extra data with `{! partial("components/card"; post) !}`.

### Filters

Filters transform values using the pipe syntax `{= value | filter =}`:

| Filter   | Description                              | Example                        |
|----------|------------------------------------------|--------------------------------|
| `md`     | Render Markdown to HTML                  | `{= post.body \| md =}`       |
| `escape` | HTML-escape (default, usually implicit)  | `{= title \| escape =}`       |
| `raw`    | Output without escaping                  | `{= html_content \| raw =}`   |
| `upper`  | Convert to uppercase                     | `{= tag \| upper =}`          |
| `lower`  | Convert to lowercase                     | `{= name \| lower =}`         |
| `trim`   | Strip leading/trailing whitespace        | `{= input \| trim =}`         |

Filters can be chained: `{= post.body | md | raw =}`.

### Directives summary

| Syntax | Purpose |
|---|---|
| `{= expr =}` | Print expression |
| `{! layout("name") !}` | Inherit from a layout template |
| `{! block("name") !}...{! end !}` | Define or fill a named block |
| `{! partial("name") !}` | Include a partial template |
| `{% if cond %}...{% endif %}` | Conditional rendering |
| `{% for x in list %}...{% endfor %}` | Loop over a collection |

---

## 5. Troubleshooting

### "template not found: base"

The layout name in `{! layout("base") !}` must match a file in `templates/`. Ensure `templates/base.tkt` exists. The name is the filename without the `.tkt` extension.

### "content directory not found"

Check that `ooke.toml` has `[content] dir = "content"` and that the `content/` directory exists at the project root.

### Blog post not appearing in listing

- Confirm the `.md` file is inside `content/posts/`
- Check that the frontmatter has valid YAML between `---` fences
- Ensure the `slug` field is set and unique
- Verify the `date` field is a valid date format (YYYY-MM-DD)

### "route not found" for /blog/hello-world

- The file `pages/blog/[slug].tk` must exist with the square brackets in the filename
- The slug in the URL must match a `slug` field in a content file
- Run `ooke build` and check the `out/` directory to see what was generated

### CSS not loading

- Static files must be in the `static/` directory
- Reference them with absolute paths in templates: `/style.css`, not `style.css`
- After `ooke build`, confirm the file was copied to `out/style.css`

### Markdown not rendering as HTML

- Use the `md` filter: `{= post.body | md =}`
- Without the filter, the raw Markdown text is printed as-is
- If you see escaped HTML tags, you may need `| md | raw` to prevent double-escaping

### Build produces empty pages

- Check that the route handler returns a response: the function must use `<` (return) with `tpl.renderfile(...)`
- Verify the template has `{! layout("base") !}` and `{! block("content") !}` correctly paired with `{! end !}`

---

## 6. Exercises

Try extending the blog on your own. These are listed roughly by difficulty.

### 6.1 Add tags to posts

Add a `tags` field to your post frontmatter:

```yaml
tags:
  - toke
  - tutorial
```