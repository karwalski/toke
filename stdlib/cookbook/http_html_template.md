# Cookbook: Dynamic Web Pages

## Overview

Serve dynamic HTML pages from an HTTP server using template rendering. Combines `std.http`, `std.html`, and `std.template` to build a server-rendered web application that returns styled HTML with data injected at request time.

## Modules Used

- `std.http` -- HTTP server and request/response handling
- `std.html` -- HTML element construction and escaping
- `std.template` -- String template compilation and rendering

## Complete Program

```toke
I std.http;
I std.html;
I std.template;
I std.file;
I std.str;

(* Load and compile the page layout template *)
let layout_src = file.read_text("templates/layout.html")!;
let layout = template.compile(layout_src)!;

(* Load and compile the home page template *)
let home_src = file.read_text("templates/home.html")!;
let home_tmpl = template.compile(home_src)!;

(* Load and compile the user profile template *)
let profile_src = file.read_text("templates/profile.html")!;
let profile_tmpl = template.compile(profile_src)!;

(* Render a full page by nesting content inside the layout *)
fn render_page(title: Str; body_html: Str): Str {
    let vars = template.new_vars();
    template.set(vars; "title"; html.escape(title));
    template.set(vars; "body"; body_html);
    template.set(vars; "year"; "2026");
    template.render(layout; vars)!;
};

(* Handler: home page with a list of features *)
fn home_page(req: http.Request): http.Response {
    let features = html.ul(@(
        html.li("Fast compilation");
        html.li("Type-safe error handling");
        html.li("Built-in standard library");
    ));

    let vars = template.new_vars();
    template.set(vars; "welcome_msg"; "Welcome to toke");
    template.set(vars; "features"; features);
    let body = template.render(home_tmpl; vars)!;

    let page = render_page("Home"; body);
    http.respond(200; page; "text/html");
};

(* Handler: user profile page *)
fn profile_page(req: http.Request; username: Str): http.Response {
    let safe_name = html.escape(username);

    let vars = template.new_vars();
    template.set(vars; "username"; safe_name);
    template.set(vars; "joined"; "2025-03-15");
    template.set(vars; "bio"; html.escape("Building things with toke."));
    let body = template.render(profile_tmpl; vars)!;

    let page = render_page(str.concat("Profile: "; safe_name); body);
    http.respond(200; page; "text/html");
};

(* Handler: 404 page *)
fn not_found(req: http.Request): http.Response {
    let body = html.div(
        html.h1("Page Not Found");
        html.p("The page you requested does not exist.");
        html.a("/"; "Return home");
    );
    let page = render_page("Not Found"; html.render(body));
    http.respond(404; page; "text/html");
};

(* Start the server *)
let server = http.listen(3000)!;
http.on(server; "GET"; "/"; home_page);
http.on(server; "GET"; "/user/:username"; profile_page);
http.fallback(server; not_found);
http.serve(server);
```

## Example Layout Template

The file `templates/layout.html` might contain:

```html
<!DOCTYPE html>
<html>
<head><title>{{title}}</title></head>
<body>
  <nav><a href="/">Home</a></nav>
  <main>{{body}}</main>
  <footer>&copy; {{year}}</footer>
</body>
</html>
```

## Step-by-Step Explanation

1. **Load templates** -- Template files are read from disk once at startup and compiled into reusable objects. This avoids re-parsing on every request.

2. **Layout wrapper** -- `render_page` injects a page title and body HTML into the shared layout. `html.escape` prevents XSS by escaping user-supplied values.

3. **Home handler** -- `html.ul` and `html.li` construct an HTML list programmatically. The `@()` syntax creates an array of list items. The rendered snippet is injected into the home template.

4. **Profile handler** -- The username comes from the URL parameter. It is escaped before insertion into the template. The profile template receives the safe name and other fields.

5. **404 fallback** -- `http.fallback` catches any unmatched route. The 404 page is built with `html.div`, `html.h1`, and other element constructors, then rendered to a string.

6. **Server startup** -- Routes are registered with `http.on` and the fallback. `http.serve` blocks and handles requests.
