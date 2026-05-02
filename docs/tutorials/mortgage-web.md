---
title: "Tutorial: Mortgage Calculator (Web App)"
slug: mortgage-web
section: tutorials
order: 2
---

Build a web-based mortgage calculator with ooke, the toke web framework. By the end you will have a form-driven app that computes monthly payments, renders a full amortisation table, and draws an SVG stacked-bar chart comparing principal versus interest by year.

**Time:** ~45 minutes
**Difficulty:** Intermediate
**Prerequisites:** toke compiler installed, ooke installed, completed [CLI tutorial](/docs/tutorials/mortgage-cli/)

---

## 1. What We're Building

The finished app has three user-facing pieces:

- **Input form** — collects principal, annual rate (decimal), loan term in years, and optional extra monthly payment.
- **Results page** — shows monthly payment, total interest, total cost, a per-year SVG chart, and the full month-by-month amortisation schedule.
- **Static stylesheet** — clean responsive layout with cards, a sticky table header, and mobile breakpoints.

The project is split across seven files:

| File | Responsibility |
|------|---------------|
| `ooke.toml` | Site metadata and build config |
| `pages/app.tk` | Router setup, static file handler, main entry point |
| `pages/index.tk` | GET `/` handler -- render the input form |
| `pages/calculate.tk` | POST `/calculate` handler -- parse form, run math, build HTML |
| `templates/layout.tkt` | Outer HTML shell with `{{content}}` slot |
| `templates/index.tkt` | Form markup |
| `templates/results.tkt` | Results page with placeholders for computed values |
| `static/style.css` | All styling |

Architecture at a glance:

```
Browser                  ooke server
  |                          |
  |-- GET / ---------------->|  index.tk handler
  |<--- HTML form -----------|  layout.tkt + index.tkt
  |                          |
  |-- POST /calculate ------>|  calculate.tk handler
  |<--- HTML results --------|  layout.tkt + results.tkt + SVG
```

---

## 2. Prerequisites

1. **toke compiler** — `toke --version` should print a version string.
2. **ooke** — `ooke --version` should print a version string.
3. **CLI tutorial done** — you should already be comfortable with modules, types, result handling, and the amortisation formula from the [CLI mortgage calculator](/docs/tutorials/mortgage-cli/).

---

## 3. LLM Prompts

If you are using an LLM to help write toke code, here are prompts tuned for each component. Feed them one at a time, review the output, and paste the result into the corresponding file.

### 3.1 Project scaffold

> Create an ooke web project called "Mortgage Calculator". The ooke.toml should set the site name, url to localhost:8080, language to "en", and build output to "build". Show only the ooke.toml file.

### 3.2 Form page handler

> Write a toke ooke page handler in module mortgage.web.index that serves a GET request. It should render templates/layout.tkt with a title variable set to "Mortgage Calculator", then render templates/index.tkt as the body, replace the {{content}} placeholder in the layout with the body, and return an HTTP 200 response. Use std.http, std.template, and std.str imports.

### 3.3 Calculation handler

> Write a toke ooke page handler in module mortgage.web.calculate that handles a POST of URL-encoded form data. It should:
> 1. Parse the request body into key=value pairs.
> 2. Extract principal (f64), rate (f64), term (u64), and extra (f64, defaulting to 0).
> 3. Validate rate is between 0 and 1, term is at least 1.
> 4. Compute monthly payment using M = P * [r(1+r)^n] / [(1+r)^n - 1].
> 5. Build an HTML amortisation table (one row per month).
> 6. Build an SVG stacked bar chart comparing principal vs interest per year.
> 7. Render the results template with all computed values and return HTTP 200.
> Use toke 56-char syntax. All names lowercase. Types prefixed with $. Arrays with @(). Return with <.

### 3.4 Router and main

> Write a toke ooke app module mortgage.web.app that sets up a router with three routes: GET / mapped to the index handler, POST /calculate mapped to the calculate handler, and GET /static/style.css mapped to a static file handler that reads from the static/ directory. The main function should start the server on 0.0.0.0:8080.

### 3.5 Templates

> Write three ooke .tkt template files for a mortgage calculator:
> 1. layout.tkt — HTML5 shell with {{title}} in the head, a navbar, a {{content}} slot in main, and a footer.
> 2. index.tkt — a form card POSTing to /calculate with fields: principal, rate, term, extra, and a submit button.
> 3. results.tkt — a results card with {{monthly}}, {{totalinterest}}, {{totalcost}} summary; an input recap; a {{chartsvg}} container; and an amortisation table with {{schedulerows}}.

### 3.6 Stylesheet

> Write a CSS file for a mortgage calculator web app. Cards with rounded corners and subtle shadow. Sticky table header. Responsive grid summary. Mobile breakpoint at 600px. Dark navbar and footer (#2c3e50). Blue accent buttons (#3498db). Year-boundary lines in the table every 12 rows.

---

## 4. Step by Step

### 4.1 Scaffold the project

```bash
ooke new mortgage-web
cd mortgage-web
```

This creates the directory with a default `ooke.toml`. Open it and set:

```toml
[site]
name = "Mortgage Calculator"
url = "http://localhost:8080"
language = "en"

[build]
output = "build"
```

Create the directory structure:

```bash
mkdir -p pages templates static
```

### 4.2 The layout template

Create `templates/layout.tkt`. This is the outer shell every page shares:

```html
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{title}}</title>
  <link rel="stylesheet" href="/static/style.css">
</head>
<body>
  <nav class="navbar">
    <a href="/" class="nav-brand">Mortgage Calculator</a>
  </nav>
  <main class="container">
    {{content}}
  </main>
  <footer class="footer">
    <p>Built with toke + ooke</p>
  </footer>
</body>
</html>
```

The `{{title}}` placeholder is filled by the handler. The `{{content}}` placeholder is replaced with the inner page template after both are rendered separately.

### 4.3 The form template

Create `templates/index.tkt`:

```html
<div class="card">
  <h1>Mortgage Calculator</h1>
  <p class="subtitle">Calculate your monthly payment and view the full
    amortisation schedule.</p>

  <form method="post" action="/calculate" class="mortgage-form">
    <div class="form-group">
      <label for="principal">Loan Amount ($)</label>
      <input type="number" id="principal" name="principal"
             value="500000" min="1" step="1000" required>
    </div>

    <div class="form-group">
      <label for="rate">Annual Interest Rate
        (decimal, e.g. 0.065 for 6.5%)</label>
      <input type="number" id="rate" name="rate"
             value="0.065" min="0" max="1" step="0.001" required>
    </div>

    <div class="form-group">
      <label for="term">Loan Term (years)</label>
      <input type="number" id="term" name="term"
             value="30" min="1" max="50" required>
    </div>

    <div class="form-group">
      <label for="extra">Extra Monthly Payment ($)</label>
      <input type="number" id="extra" name="extra"
             value="0" min="0" step="50">
    </div>

    <button type="submit" class="btn">Calculate</button>
  </form>
</div>
```

Note the form POSTs to `/calculate` as URL-encoded data.

### 4.4 The index handler

Create `pages/index.tk`:

```toke
m=mortgage.web.index;
i=http:std.http;
i=tpl:std.template;
i=str:std.str;

f=handler(req:http.$req):http.$res{
  let vars=tpl.vars(@("title":"Mortgage Calculator"));
  let layout=mt tpl.renderfile("templates/layout.tkt";vars) {
    $ok:v v;
    $err:e <http.res.err("template error")
  };
  let body=mt tpl.renderfile("templates/index.tkt";tpl.vars(@())) {
    $ok:v v;
    $err:e <http.res.err("template error")
  };
  let page=str.replace(layout;"{{content}}";body);
  <http.res.ok(page)
};
```

Key points:

- `tpl.renderfile` returns a result type. The `mt` match handles both `$ok` and `$err`.
- The layout and body are rendered independently, then composed with `str.replace`. This two-pass approach keeps templates simple — no nested include directives needed.
- `tpl.vars(@("title":"Mortgage Calculator"))` creates a string-keyed map of template variables.

### 4.5 The calculation handler

Create `pages/calculate.tk`. This is the largest file. We will walk through it function by function.

#### 4.5.1 Module header and imports

```toke
m=mortgage.web.calculate;
i=http:std.http;
i=tpl:std.template;
i=str:std.str;
i=math:std.math;
i=svg:std.svg;
```

Five imports: HTTP primitives, template rendering, string manipulation, math (for `pow`), and SVG generation.

#### 4.5.2 Form parsing

```toke
f=parseform(body:str):@(str:str){
  let pairs=str.split(body;"&");
  let result=mut.@();
  lp(let i=0;i<(pairs.len as i64);i=i+1){
    let pair=pairs.get(i);
    let kv=str.split(pair;"=");
    if(kv.len>1){
      let k=kv.get(0);
      let v=kv.get(1);
      result=result.push(@(k:v))
    }
  };
  <result
};

f=formval(form:@(str:str);key:str):str{
  lp(let i=0;i<(form.len as i64);i=i+1){
    let entry=form.get(i);
    if(entry.key==key){
      <entry.val
    }
  };
  <"0"
};
```

`parseform` splits the URL-encoded body on `&`, then each pair on `=`. It builds an array of key-value pairs. `formval` does a linear scan for a key, returning `"0"` as the default — good enough for numeric fields.

#### 4.5.3 Monthly payment formula

```toke
f=monthlypayment(principal:f64;rate:f64;months:u64):f64{
  let r=rate/12.0;
  if(r<0.000000001){
    <principal/(months as f64)
  };
  let rn=math.pow(1.0+r;(months as f64));
  <principal*(r*rn)/(rn-1.0)
};
```

The zero-rate guard prevents division by zero. When the rate is effectively zero, simple division gives the right answer.

#### 4.5.4 Money formatting

```toke
f=formatmoney(v:f64):str{
  let whole=(v as i64);
  let frac=((v-(whole as f64))*100.0+0.5 as i64);
  if(frac>99){
    whole=whole+1;
    frac=0
  };
  let b=str.buf();
  str.add(b;str.fromint(whole));
  str.add(b;".");
  if(frac<10){
    str.add(b;"0")
  };
  str.add(b;str.fromint(frac));
  <str.done(b)
};
```

Manual two-decimal formatting. The `+0.5` before the cast rounds rather than truncating. The `frac>99` guard handles the carry when rounding pushes cents to 100.

#### 4.5.5 Amortisation table

```toke
f=buildschedulerows(principal:f64;rate:f64;term:u64;
    extra:f64;pmt:f64):str{
  let n=term*12;
  let bal=mut.principal;
  let b=str.buf();
  let totalinterest=mut.0.0;
  let totalpaid=mut.0.0;
  lp(let i=1;i<(n as i64)+1;i=i+1){
    let mi=bal*(rate/12.0);
    let mp=pmt+extra;
    if(mp>bal+mi){
      mp=bal+mi
    };
    let princ=mp-mi;
    bal=bal-princ;
    if(bal<0.0){bal=0.0};
    totalinterest=totalinterest+mi;
    totalpaid=totalpaid+mp;
    str.add(b;"<tr><td>");
    str.add(b;str.fromint(i));
    str.add(b;"</td><td>$");
    str.add(b;formatmoney(mp));
    str.add(b;"</td><td>$");
    str.add(b;formatmoney(princ));
    str.add(b;"</td><td>$");
    str.add(b;formatmoney(mi));
    str.add(b;"</td><td>$");
    str.add(b;formatmoney(bal));
    str.add(b;"</td></tr>");
    if(bal<0.01){
      <str.done(b)
    }
  };
  <str.done(b)
};
```

The loop uses a string buffer (`str.buf()`) for efficient concatenation. It caps the final payment to the remaining balance plus interest, so the last row always shows a zero balance. When extra payments are large enough to pay off the loan early, the `bal<0.01` guard exits the loop.

#### 4.5.6 SVG chart

```toke
f=buildsvgchart(principal:f64;rate:f64;term:u64;
    extra:f64;pmt:f64):str{
  let n=term*12;
  let bal=mut.principal;
  let years=(term as i64);
  let chartw=700.0;
  let charth=300.0;
  let marginl=60.0;
  let marginb=40.0;
  let barw=chartw/(years as f64)-4.0;
  if(barw>40.0){barw=40.0};
  if(barw<4.0){barw=4.0};

  let yearlyprinc=mut.@();
  let yearlyint=mut.@();
  let maxannual=mut.0.0;
  let yrprinc=mut.0.0;
  let yrint=mut.0.0;
  lp(let i=1;i<(n as i64)+1;i=i+1){
    let mi=bal*(rate/12.0);
    let mp=pmt+extra;
    if(mp>bal+mi){mp=bal+mi};
    let princ=mp-mi;
    bal=bal-princ;
    if(bal<0.0){bal=0.0};
    yrprinc=yrprinc+princ;
    yrint=yrint+mi;
    if(i>(0 as i64)&&(i as u64)%12==0||bal<0.01){
      let total=yrprinc+yrint;
      if(total>maxannual){maxannual=total};
      yearlyprinc=yearlyprinc.push(yrprinc);
      yearlyint=yearlyint.push(yrint);
      yrprinc=0.0;
      yrint=0.0;
      if(bal<0.01){
        lp(let j=(yearlyprinc.len as i64);j<years;j=j+1){
          yearlyprinc=yearlyprinc.push(0.0);
          yearlyint=yearlyint.push(0.0)
        };
        i=(n as i64)+1
      }
    }
  };

  let d=svg.doc(chartw+marginl+20.0;charth+marginb+40.0);
  let princstyle=svg.style("#4caf50";"none";0.0);
  let intstyle=svg.style("#f44336";"none";0.0);
  let axisstyle=svg.style("none";"#333";1.0);
  ...
  <svg.render(d)
};
```

The chart uses a two-pass approach. The first pass accumulates yearly principal and interest totals and finds the maximum for scaling. The second pass draws the bars. Green (`#4caf50`) for principal, red (`#f44336`) for interest. The `std.svg` module provides `svg.doc`, `svg.rect`, `svg.line`, `svg.text`, and `svg.render` for building SVG documents programmatically.

See the full source in `examples/mortgage-web/pages/calculate.tk` for the complete SVG rendering including axis labels and legend.

#### 4.5.7 The handler

```toke
f=handler(req:http.$req):http.$res{
  let form=parseform(req.body);
  let principals=formval(form;"principal");
  let rates=formval(form;"rate");
  let terms=formval(form;"term");
  let extras=formval(form;"extra");

  let principal=mt str.tofloat(principals) {
    $ok:v v;
    $err:e <http.res.bad("invalid principal")
  };
  let annualrate=mt str.tofloat(rates) {
    $ok:v v;
    $err:e <http.res.bad("invalid rate")
  };
  let term=mt str.toint(terms) {
    $ok:v (v as u64);
    $err:e <http.res.bad("invalid term")
  };
  let extra=mt str.tofloat(extras) {
    $ok:v v;
    $err:e 0.0
  };

  if(annualrate<0.0||annualrate>1.0){
    <http.res.bad("rate must be between 0 and 1 (e.g. 0.065 for 6.5%)")
  };
  if(term<1){
    <http.res.bad("term must be at least 1 year")
  };

  let pmt=monthlypayment(principal;annualrate;term*12);
  let totals=calctotals(principal;annualrate;term;extra;pmt);
  let totalinterest=totals.get(0);
  let totalcost=totals.get(1);

  let schedulehtml=buildschedulerows(
    principal;annualrate;term;extra;pmt);
  let chartsvg=buildsvgchart(
    principal;annualrate;term;extra;pmt);

  let vars=tpl.vars(@(
    "monthly":str.concat("$";formatmoney(pmt+extra));
    "totalinterest":str.concat("$";formatmoney(totalinterest));
    "totalcost":str.concat("$";formatmoney(totalcost));
    "principal":formatmoney(principal);
    "rate":rates;
    "term":str.fromint(term as i64);
    "extra":formatmoney(extra);
    "schedulerows":schedulehtml;
    "chartsvg":chartsvg
  ));

  let body=mt tpl.renderfile("templates/results.tkt";vars) {
    $ok:v v;
    $err:e <http.res.err("template error")
  };
  let layoutvars=tpl.vars(@("title":"Mortgage Results"));
  let layout=mt tpl.renderfile("templates/layout.tkt";layoutvars) {
    $ok:v v;
    $err:e <http.res.err("template error")
  };
  let page=str.replace(layout;"{{content}}";body);
  <http.res.ok(page)
};
```

The flow: parse the form body, validate inputs, run all computations, build the template variable map, render the results template, slot it into the layout, and return the complete page.

Note how `extra` defaults to `0.0` on parse failure rather than returning an error — this makes the extra payment field truly optional.

### 4.6 The results template

Create `templates/results.tkt`:

```html
<div class="card">
  <h1>Mortgage Results</h1>

  <div class="summary">
    <div class="summary-item">
      <span class="summary-label">Monthly Payment</span>
      <span class="summary-value">{{monthly}}</span>
    </div>
    <div class="summary-item">
      <span class="summary-label">Total Interest</span>
      <span class="summary-value">{{totalinterest}}</span>
    </div>
    <div class="summary-item">
      <span class="summary-label">Total Cost</span>
      <span class="summary-value">{{totalcost}}</span>
    </div>
  </div>

  <div class="input-recap">
    <p>Principal: ${{principal}} | Rate: {{rate}} |
       Term: {{term}} years | Extra: ${{extra}}/mo</p>
  </div>
</div>

<div class="card">
  <h2>Payment Breakdown by Year</h2>
  <div class="chart-container">
    {{chartsvg}}
  </div>
</div>

<div class="card">
  <h2>Amortisation Schedule</h2>
  <div class="table-wrap">
    <table class="schedule-table">
      <thead>
        <tr>
          <th>Month</th>
          <th>Payment</th>
          <th>Principal</th>
          <th>Interest</th>
          <th>Balance</th>
        </tr>
      </thead>
      <tbody>
        {{schedulerows}}
      </tbody>
    </table>
  </div>
</div>

<div class="actions">
  <a href="/" class="btn btn-secondary">Recalculate</a>
</div>
```

The `{{chartsvg}}` placeholder receives raw SVG markup — no escaping needed since we control the output. The `{{schedulerows}}` placeholder receives pre-built `<tr>` elements.

### 4.7 The router

Create `pages/app.tk`:

```toke
m=mortgage.web.app;
i=router:std.router;
i=http:std.http;
i=file:std.file;
i=str:std.str;
i=idx:mortgage.web.index;
i=calc:mortgage.web.calculate;

f=statichandler(req:http.$req):http.$res{
  let path=str.concat("static";req.path);
  let content=mt file.read(path) {
    $ok:v v;
    $err:e <http.res.bad("file not found")
  };
  <$http.$res{
    status:200;
    headers:@(@("content-type":"text/css"));
    body:content
  }
};

f=main():i64{
  let r=router.new();
  router.get(r;"/";idx.handler);
  router.post(r;"/calculate";calc.handler);
  router.get(r;"/static/style.css";statichandler);
  mt router.serve(r;"0.0.0.0";8080) {
    $ok:_ ();
    $err:e ()
  };
  <0
};
```

Three routes:

| Method | Path | Handler |
|--------|------|---------|
| GET | `/` | `idx.handler` — serve the form |
| POST | `/calculate` | `calc.handler` — process the form |
| GET | `/static/style.css` | `statichandler` — serve the stylesheet |

The `statichandler` constructs the file path by concatenating `"static"` with `req.path` (which is `/style.css`), giving `static/style.css`. It returns a response with an explicit `content-type` header.

### 4.8 The stylesheet

Create `static/style.css`. The full file is in the example at `examples/mortgage-web/static/style.css`. Highlights:

- **Cards** — white background, 8px border radius, subtle box shadow.
- **Sticky header** — `.schedule-table thead` uses `position: sticky; top: 0` so column headers stay visible while scrolling the amortisation table.
- **Year boundaries** — `.schedule-table tbody tr:nth-child(12n)` gets a blue bottom border to visually separate years.
- **Responsive** — at 600px the summary grid collapses to a single column and table font size shrinks.

### 4.9 Serve the app

```bash
ooke serve
```

Open `http://localhost:8080` in your browser. You should see the mortgage calculator form. Fill in the defaults and click **Calculate** to see the results page with the SVG chart and amortisation table.

To build a static version:

```bash
ooke build
```

The output goes to the `build/` directory as specified in `ooke.toml`.

---

## 5. Testing with curl

You can exercise the app from the command line without a browser.

### 5.1 Load the form

```bash
curl -s http://localhost:8080/ | head -20
```

You should see the HTML form markup.

### 5.2 Submit default values

```bash
curl -s -X POST http://localhost:8080/calculate \
  -d "principal=500000&rate=0.065&term=30&extra=0"
```

Look for the summary section in the response. The monthly payment for a $500,000 loan at 6.5% over 30 years should be approximately $3,160.34.

### 5.3 Submit with extra payments

```bash
curl -s -X POST http://localhost:8080/calculate \
  -d "principal=500000&rate=0.065&term=30&extra=500"
```

With $500/month extra, total interest drops significantly and the loan pays off early. The amortisation table will have fewer than 360 rows.

### 5.4 Edge cases

Zero interest rate:

```bash
curl -s -X POST http://localhost:8080/calculate \
  -d "principal=120000&rate=0&term=10&extra=0"
```

Monthly payment should be exactly $1,000.00 ($120,000 / 120 months).

Invalid rate (should return 400):

```bash
curl -s -X POST http://localhost:8080/calculate \
  -d "principal=500000&rate=1.5&term=30&extra=0"
```

Expected response: `rate must be between 0 and 1 (e.g. 0.065 for 6.5%)`.

---

## 6. Troubleshooting

### Template not found

```
template error
```

Make sure template paths in `tpl.renderfile` are relative to the project root (where `ooke.toml` lives), not relative to the `pages/` directory. The correct path is `"templates/layout.tkt"`, not `"../templates/layout.tkt"`.

### Form values are all zero

If every parsed value comes back as `"0"`, check that:

1. The form `method` is `post` (not `get`).
2. The form `action` is `/calculate`.
3. Each input has a `name` attribute matching the key used in `formval` (e.g., `name="principal"`).

### Math precision issues

The `formatmoney` function uses integer truncation with a `+0.5` rounding offset. For very large loans (above ~$10M), floating-point precision in `f64` can cause the last cent to drift. This is acceptable for a calculator demo. For production financial software, use a fixed-point decimal library.

### SVG chart not rendering

If the chart area is blank, check:

1. The `std.svg` import is present.
2. `maxannual` is not zero — this happens if the rate and principal are both zero.
3. The `{{chartsvg}}` placeholder in `results.tkt` is inside a `<div>` (not inside a `<p>`, which cannot contain block elements).

### Port already in use

```
error: address already in use: 0.0.0.0:8080
```

Kill the previous instance or change the port in `app.tk`:

```toke
mt router.serve(r;"0.0.0.0";3000) {
```

---

## 7. Exercises

Once the basic calculator is working, try extending it.

### 7.1 Comparison mode

Add a second form that lets the user enter two sets of loan parameters side by side. Render both results on the same page with two SVG charts and a comparison summary showing the difference in total interest and payoff time.

Hints:
- Add a new route `POST /compare` with its own handler.
- Create a `templates/compare.tkt` with two chart containers.
- Reuse `monthlypayment`, `buildsvgchart`, and the other computation functions from `calculate.tk` by importing the module.

### 7.2 PDF export

Add a `/calculate/pdf` endpoint that returns the results as a downloadable PDF instead of HTML.

Hints:
- Use `std.pdf` (if available) or generate a minimal PDF manually with string buffers.
- Set the response `content-type` header to `"application/pdf"`.
- Add a "Download PDF" button to the results template that links to the PDF endpoint with the same parameters as query string values.

### 7.3 LocalStorage persistence

Add a small inline `<script>` block to the form template that saves the last-used input values to `localStorage` and restores them on page load.

Hints:
- Add the script directly in `index.tkt` after the form.
- On form submit, save each input value to `localStorage.setItem("mortgage_" + name, value)`.
- On page load, read from `localStorage` and set each input's value.
- This is pure client-side JavaScript — no toke changes needed.

---

## What's Next

You now have a working web application built with ooke. The patterns you have learned — route handlers, template composition, form parsing, and server-side SVG generation — apply to any ooke web project.

Suggested next tutorials:

- [REST API](/docs/tutorials/rest-api/) — build a JSON API with ooke
- [Static Site](/docs/tutorials/static-site/) — generate a static site with ooke
- [Cross-Platform](/docs/tutorials/cross-platform/) — share code between CLI and web targets
