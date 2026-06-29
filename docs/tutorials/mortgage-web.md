---
title: "Tutorial: Mortgage Calculator (Web App)"
slug: mortgage-web
section: tutorials
order: 2
---

Build a web-based mortgage calculator in toke. It uses `std.router` for routing, `std.template` for HTML, `std.svg` for a server-rendered chart, and `std.file` to serve static assets — composed as a plain multi-module toke binary. By the end you will have a form-driven app that computes monthly payments, renders a full amortisation table, and draws an SVG stacked-bar chart comparing principal versus interest by year.

**Time:** ~45 minutes
**Difficulty:** Intermediate
**Prerequisites:** toke compiler installed, completed [CLI tutorial](/docs/tutorials/mortgage-cli/)

---

## 1. What We're Building

The finished app has three user-facing pieces:

- **Input form** — collects principal, annual rate (decimal), loan term in years, and optional extra monthly payment.
- **Results page** — shows monthly payment, total interest, total cost, a per-year SVG chart, and the full month-by-month amortisation schedule.
- **Static stylesheet** — clean responsive layout with cards, a sticky table header, and mobile breakpoints.

The project is split across seven files:

| File | Responsibility |
|------|---------------|
| `ooke.toml` | Optional project metadata (not required to build or run) |
| `pages/app.tk` | Router setup, static file handler, main entry point |
| `pages/index.tk` | GET `/` handler -- render the input form |
| `pages/calculate.tk` | POST `/calculate` handler -- parse form, run math, build HTML |
| `templates/layout.tkt` | Outer HTML shell with `{{content}}` slot |
| `templates/index.tkt` | Form markup |
| `templates/results.tkt` | Results page with placeholders for computed values |
| `static/style.css` | All styling |

Architecture at a glance:

```
Browser                  toke server
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
2. **CLI tutorial done** — you should already be comfortable with modules, types, result handling, and the amortisation formula from the [CLI mortgage calculator](/docs/tutorials/mortgage-cli/).

---

## 3. LLM Prompts

If you are using an LLM to help write toke code, here are prompts tuned for each component. Feed them one at a time, review the output, and paste the result into the corresponding file.

### 3.1 Project scaffold

> Scaffold a toke web project called "Mortgage Calculator": a `pages/` directory for handler modules, `templates/` for `.tkt` files, and `static/` for assets. (An optional `ooke.toml` can hold project metadata.)

### 3.2 Form page handler

> Write a toke page handler in module mortgage.web.index that serves a GET request. It should render templates/layout.tkt with a title variable set to "Mortgage Calculator", then render templates/index.tkt as the body, replace the {{content}} placeholder in the layout with the body, and return an HTTP 200 response. Use std.http, std.template, and std.str imports.

### 3.3 Calculation handler

> Write a toke page handler in module mortgage.web.calculate that handles a POST of URL-encoded form data. It should:
> 1. Parse the request body into key=value pairs.
> 2. Extract principal (f64), rate (f64), term (u64), and extra (f64, defaulting to 0).
> 3. Validate rate is between 0 and 1, term is at least 1.
> 4. Compute monthly payment using M = P * [r(1+r)^n] / [(1+r)^n - 1].
> 5. Build an HTML amortisation table (one row per month).
> 6. Build an SVG stacked bar chart comparing principal vs interest per year.
> 7. Render the results template with all computed values and return HTTP 200.
> Use toke 55-char syntax. All names lowercase. Types prefixed with $. Arrays with @(). Return with <.

### 3.4 Router and main

> Write a toke app module mortgage.web.app that sets up a router with three routes: GET / mapped to the index handler, POST /calculate mapped to the calculate handler, and GET /static/style.css mapped to a static file handler that reads from the static/ directory. The main function should start the server on 0.0.0.0:8080.

### 3.5 Templates

> Write three `.tkt` template files for a mortgage calculator:
> 1. layout.tkt — HTML5 shell with {{title}} in the head, a navbar, a {{content}} slot in main, and a footer.
> 2. index.tkt — a form card POSTing to /calculate with fields: principal, rate, term, extra, and a submit button.
> 3. results.tkt — a results card with {{monthly}}, {{total_interest}}, {{total_cost}} summary; an input recap; a {{chart_svg}} container; and an amortisation table with {{schedule_rows}}.

### 3.6 Stylesheet

> Write a CSS file for a mortgage calculator web app. Cards with rounded corners and subtle shadow. Sticky table header. Responsive grid summary. Mobile breakpoint at 600px. Dark navbar and footer (#2c3e50). Blue accent buttons (#3498db). Year-boundary lines in the table every 12 rows.

---

## 4. Step by Step

### 4.1 Scaffold the project

```bash
mkdir mortgage-web
cd mortgage-web
```

Optionally create an `ooke.toml` with project metadata (it is not read by the `std.router` binary, but documents the project):

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
    <p>Built with toke</p>
  </footer>
</body>
</html>
```

The `{{title}}` and `{{content}}` placeholders are both filled by the handler: it renders the inner page first, then renders the layout with `content` set to that rendered body (the template engine substitutes unknown placeholders with empty, so the body must be passed as a variable rather than spliced in afterwards).

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
f=handler(req:i64):i64{
  let body=mt tpl.renderfile("templates/index.tkt";tpl.vars(@())) {
    $ok:v v;
    $err:e <router.status(500;"template error")
  };
  let vars=tpl.vars(@("title":"Mortgage Calculator";"content":body));
  let page=mt tpl.renderfile("templates/layout.tkt";vars) {
    $ok:v v;
    $err:e <router.status(500;"template error")
  };
  <router.ok(page)
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
i=router:std.router;
i=tpl:std.template;
i=str:std.str;
i=math:std.math;
i=svg:std.svg;
```

Five imports: HTTP primitives, template rendering, string manipulation, math (for `pow`), and SVG generation.

#### 4.5.2 Form parsing

```toke
(* look up a urlencoded form field directly from the request body *)
f=formval(body:str;key:str):str{
  let pairs=str.split(body;"&");
  lp(let i=0;i<(pairs.len as i64);i=i+1){
    let kv=str.split(pairs.get(i);"=");
    if(kv.len>1){
      if(kv.get(0)=key){
        <kv.get(1)
      }
    }
  };
  <"0"
};
```

`formval` splits the URL-encoded body on `&`, then each pair on `=`, and returns the value for the requested key (or `"0"` as the default). Reading fields straight off the body string keeps the request handling simple — only strings cross the function boundary.

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
  let whole=mut.(v as i64);
  let frac=mut.(((v-(whole as f64))*100.0+0.5) as i64);
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
f=buildschedulerows(principal:f64;rate:f64;term:u64;extra:f64;pmt:f64):str{
  let n=term*12;
  let bal=mut.principal;
  let b=str.buf();
  let totalinterest=mut.0.0;
  let totalpaid=mut.0.0;
  lp(let i=1;i<(n as i64)+1;i=i+1){
    let mi=bal*(rate/12.0);
    let mp=mut.(pmt+extra);
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
f=buildsvgchart(principal:f64;rate:f64;term:u64;extra:f64;pmt:f64):str{
  let n=term*12;
  let bal=mut.principal;
  let years=(term as i64);
  let chartw=700.0;
  let charth=300.0;
  let marginl=60.0;
  let marginb=40.0;
  let barw=mut.(chartw/(years as f64)-4.0);
  if(barw>40.0){barw=40.0};
  if(barw<4.0){barw=4.0};

  
  let yearlyprinc=mut.@();
  let yearlyint=mut.@();
  let maxannual=mut.0.0;
  let yrprinc=mut.0.0;
  let yrint=mut.0.0;
  lp(let i=1;i<(n as i64)+1;i=i+1){
    let mi=bal*(rate/12.0);
    let mp=mut.(pmt+extra);
    if(mp>bal+mi){mp=bal+mi};
    let princ=mp-mi;
    bal=bal-princ;
    if(bal<0.0){bal=0.0};
    yrprinc=yrprinc+princ;
    yrint=yrint+mi;
    if(i>(0 as i64)&&(i as u64)%12=0||bal<0.01){
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
        i=(n as i64)+1;
      }
    }
  };

  
  let d=mut.svg.doc(chartw+marginl+20.0;charth+marginb+40.0);
  let princstyle=svg.style("#4caf50";"none";0.0);
  let intstyle=svg.style("#f44336";"none";0.0);
  let axisstyle=svg.style("none";"#333";1.0);
  let txtstyle=$svgstyle{fill:"#555";stroke:"none";strokewidth:0.0;opacity:1.0;fontsize:10.0;fontfamily:"sans-serif"};
  let titlestyle=$svgstyle{fill:"#333";stroke:"none";strokewidth:0.0;opacity:1.0;fontsize:14.0;fontfamily:"sans-serif"};

  
  d=svg.append(d;svg.text(marginl+chartw/2.0-80.0;16.0;"principal vs interest by year";titlestyle));

  
  d=svg.append(d;svg.line(marginl;30.0;marginl;charth+30.0;axisstyle));
  
  d=svg.append(d;svg.line(marginl;charth+30.0;marginl+chartw;charth+30.0;axisstyle));

  if(maxannual<1.0){maxannual=1.0};
  let scale=charth/maxannual;
  let actualyears=(yearlyprinc.len as i64);
  let spacing=chartw/(actualyears as f64);

  lp(let y=0;y<actualyears;y=y+1){
    let yp=yearlyprinc.get(y);
    let yi=yearlyint.get(y);
    let x=marginl+(y as f64)*spacing+spacing/2.0-barw/2.0;

    
    let ih=yi*scale;
    let iy=charth+30.0-ih;
    d=svg.append(d;svg.rect(x;iy;barw;ih;intstyle));

    
    let ph=yp*scale;
    let py=iy-ph;
    d=svg.append(d;svg.rect(x;py;barw;ph;princstyle));

    
    let lx=marginl+(y as f64)*spacing+spacing/2.0-4.0;
    d=svg.append(d;svg.text(lx;charth+44.0;str.fromint(y+1);txtstyle))
  };

  
  let legy=charth+60.0;
  d=svg.append(d;svg.rect(marginl;legy;12.0;12.0;princstyle));
  d=svg.append(d;svg.text(marginl+16.0;legy+10.0;"principal";txtstyle));
  d=svg.append(d;svg.rect(marginl+100.0;legy;12.0;12.0;intstyle));
  d=svg.append(d;svg.text(marginl+116.0;legy+10.0;"interest";txtstyle));

  <svg.render(d)
};
```

The chart uses a two-pass approach. The first pass accumulates yearly principal and interest totals and finds the maximum for scaling. The second pass draws the bars. Green (`#4caf50`) for principal, red (`#f44336`) for interest. The `std.svg` module provides `svg.doc`, `svg.rect`, `svg.line`, `svg.text`, and `svg.render` for building SVG documents programmatically.

See the full source in `examples/mortgage-web/pages/calculate.tk` for the complete SVG rendering including axis labels and legend.

#### 4.5.7 The handler

```toke
f=calctotals(principal:f64;rate:f64;term:u64;extra:f64;pmt:f64):@f64{
  let n=term*12;
  let bal=mut.principal;
  let totalinterest=mut.0.0;
  let totalpaid=mut.0.0;
  lp(let i=1;i<(n as i64)+1;i=i+1){
    let mi=bal*(rate/12.0);
    let mp=mut.(pmt+extra);
    if(mp>bal+mi){mp=bal+mi};
    let princ=mp-mi;
    bal=bal-princ;
    if(bal<0.0){bal=0.0};
    totalinterest=totalinterest+mi;
    totalpaid=totalpaid+mp;
    if(bal<0.01){
      <@(totalinterest;totalpaid)
    }
  };
  <@(totalinterest;totalpaid)
};


f=handler(req:i64):i64{
  let body=router.reqbody(req);
  let principals=formval(body;"principal");
  let rates=formval(body;"rate");
  let terms=formval(body;"term");
  let extras=formval(body;"extra");

  let principal=mt str.tofloat(principals) {
    $ok:v v;
    $err:e <router.bad("invalid principal")
  };
  let annualrate=mt str.tofloat(rates) {
    $ok:v v;
    $err:e <router.bad("invalid rate")
  };
  let term=mt str.toint(terms) {
    $ok:v (v as u64);
    $err:e <router.bad("invalid term")
  };
  let extra=mt str.tofloat(extras) {
    $ok:v v;
    $err:e 0.0
  };

  if(annualrate<0.0||annualrate>1.0){
    <router.bad("rate must be between 0 and 1 (e.g. 0.065 for 6.5%)")
  };
  if(term<1){
    <router.bad("term must be at least 1 year")
  };

  let pmt=monthlypayment(principal;annualrate;term*12);
  let totals=calctotals(principal;annualrate;term;extra;pmt);
  let totalinterest=totals.get(0);
  let totalcost=totals.get(1);

  let schedulehtml=buildschedulerows(principal;annualrate;term;extra;pmt);
  let chartsvg=buildsvgchart(principal;annualrate;term;extra;pmt);

  let vars=tpl.vars(@(
    "monthly":str.concat("$";formatmoney(pmt+extra));
    "total_interest":str.concat("$";formatmoney(totalinterest));
    "total_cost":str.concat("$";formatmoney(totalcost));
    "principal":formatmoney(principal);
    "rate":rates;
    "term":str.fromint(term as i64);
    "extra":formatmoney(extra);
    "schedule_rows":schedulehtml;
    "chart_svg":chartsvg
  ));

  let body=mt tpl.renderfile("templates/results.tkt";vars) {
    $ok:v v;
    $err:e <router.status(500;"template error")
  };
  let layoutvars=tpl.vars(@("title":"Mortgage Results";"content":body));
  let page=mt tpl.renderfile("templates/layout.tkt";layoutvars) {
    $ok:v v;
    $err:e <router.status(500;"template error")
  };
  <router.ok(page)
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
      <span class="summary-value">{{total_interest}}</span>
    </div>
    <div class="summary-item">
      <span class="summary-label">Total Cost</span>
      <span class="summary-value">{{total_cost}}</span>
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
    {{chart_svg}}
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
        {{schedule_rows}}
      </tbody>
    </table>
  </div>
</div>

<div class="actions">
  <a href="/" class="btn btn-secondary">Recalculate</a>
</div>
```

The `{{chart_svg}}` placeholder receives raw SVG markup — no escaping needed since we control the output. The `{{schedule_rows}}` placeholder receives pre-built `<tr>` elements.

### 4.7 The router

Create `pages/app.tk`:

```toke
m=mortgage.web.app;
i=router:std.router;
i=file:std.file;
i=idx:mortgage.web.index;
i=calc:mortgage.web.calculate;


f=statichandler(req:i64):i64{
  let content=mt file.read("static/style.css") {
    $ok:v v;
    $err:e <router.notfound("file not found")
  };
  <router.css(content)
};


f=main():i64{
  let r=router.new();
  router.get(r;"/";&idx.handler);
  router.post(r;"/calculate";&calc.handler);
  router.get(r;"/static/style.css";&statichandler);
  router.serve(r;"0.0.0.0";8080);
  <0
};
```

Three routes:

| Method | Path | Handler |
|--------|------|---------|
| GET | `/` | `idx.handler` — serve the form |
| POST | `/calculate` | `calc.handler` — process the form |
| GET | `/static/style.css` | `statichandler` — serve the stylesheet |

The `statichandler` reads `static/style.css` and returns it with `router.css`, which sets the `text/css` content-type. Routes are registered in `main` with `router.get`/`router.post`, passing each handler by reference (`&idx.handler`); `router.serve` then starts the server.

### 4.8 The stylesheet

Create `static/style.css`. The full file is in the example at `examples/mortgage-web/static/style.css`. Highlights:

- **Cards** — white background, 8px border radius, subtle box shadow.
- **Sticky header** — `.schedule-table thead` uses `position: sticky; top: 0` so column headers stay visible while scrolling the amortisation table.
- **Year boundaries** — `.schedule-table tbody tr:nth-child(12n)` gets a blue bottom border to visually separate years.
- **Responsive** — at 600px the summary grid collapses to a single column and table font size shrinks.

### 4.9 Build and serve the app

Compile the three page modules into one binary, then run it:

```bash
tkc pages/index.tk pages/calculate.tk pages/app.tk -o mortgage-web
./mortgage-web
```

`app.tk`'s `main` registers the routes and calls `router.serve(...; 8080)`,
which blocks while serving. Open `http://localhost:8080` in your browser — you
should see the mortgage calculator form. Fill in the defaults and click
**Calculate** to see the results page with the SVG chart and amortisation table.
(Run it from the project root so the relative `templates/` and `static/` paths
resolve.)

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

Make sure template paths in `tpl.renderfile` are relative to the directory you run the binary from (the project root), not relative to the `pages/` directory. The correct path is `"templates/layout.tkt"`, not `"../templates/layout.tkt"`.

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
3. The `{{chart_svg}}` placeholder in `results.tkt` is inside a `<div>` (not inside a `<p>`, which cannot contain block elements).

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

You now have a working toke web application. The patterns you have learned — `std.router` route handlers, `std.template` composition, form parsing, and server-side SVG generation — apply to any toke web project.

Suggested next tutorials:

- [REST API](/docs/tutorials/rest-api/) — build a JSON API in toke
- [Static Site](/docs/tutorials/static-site/) — generate a static site in toke
- [Cross-Platform](/docs/tutorials/cross-platform/) — share code between CLI and web targets
