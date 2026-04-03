```
Independent Submission                                       M. Karwalski
Request for Comments: draft-karwalski-toke-lang-00          tokelang.dev
Category: Informational                                       March 2026
ISSN: pending


          toke: A Machine-Native Programming Language for
              LLM-Driven Code Generation and Execution

Abstract

   This document defines toke (written shorthand: tk), a statically
   typed, compiled programming language designed for reliable generation,
   validation, and execution by large language models (LLMs) and other
   automated code-generating systems.  toke optimises for token
   efficiency, syntactic unambiguity, deterministic parsing, and
   machine-readable compiler diagnostics.  It compiles to native machine
   code via LLVM with no runtime dependency.

   toke uses a 56-character source alphabet: lowercase letters, digits,
   and 20 symbols.  Uppercase letters are eliminated through sigil-
   based encoding ($type for type names, @() for array literals),
   yielding highly predictable co-occurrence patterns that merge
   efficiently under BPE tokenization.

   The language was developed iteratively: an initial 80-character
   development profile (using uppercase keywords and bracket arrays)
   was used to bootstrap the compiler, generate the training corpus,
   and validate the core thesis.  The 56-character production profile
   described in this document is the result of that process and is the
   normative specification of the toke language.

   This document describes the language design rationale, character set
   specification, core language constructs, memory model, structured
   error protocol, compilation pipeline, corpus generation methodology,
   model training approach, validation workstreams, project phases, cost
   model, and governance framework.

Status of This Memo

   This document is an independent submission to the RFC series.  It
   represents the views of its author and does not constitute an IETF
   standard.  It is published for informational purposes and as a basis
   for community review and implementation.

   This document is not a candidate for any level of Internet Standard.
   Distribution of this document is unlimited.

Copyright Notice

   Copyright (c) 2026 M. Karwalski.  All rights reserved.

   This document is made available under the Creative Commons
   Attribution 4.0 International License (CC BY 4.0).

Table of Contents

   1.  Introduction ................................................  3
   1.1.  Motivation ................................................  3
   1.2.  Core Thesis ...............................................  3
   1.3.  Scope of This Document ....................................  4
   2.  Conventions and Terminology .................................  4
   3.  Language Identity ...........................................  5
   3.1.  Name and Shorthand ........................................  5
   3.2.  Namespace Conflict Analysis ...............................  5
   4.  Design Principles ...........................................  6
   4.1.  Machine-First Syntax ......................................  6
   4.2.  Deterministic Structure ...................................  6
   4.3.  Token Efficiency ..........................................  6
   4.4.  Strong Explicit Typing ....................................  6
   4.5.  Structured Failure ........................................  6
   4.6.  Incremental Compilability .................................  7
   4.7.  No Hidden Behaviour .......................................  7
   4.8.  Native Execution Target ...................................  7
   4.9.  Arena Memory Discipline ...................................  7
   5.  Character Set Specification .................................  7
   5.1.  The 56-Character Alphabet .................................  8
   5.2.  Development Profile (80 Characters) — Historical ..........  9
   5.3.  Token Count Estimates ..................................... 10
   6.  Symbol Assignment ........................................... 10
   6.1.  Operator and Delimiter Table .............................. 11
   6.2.  Keywords .................................................. 11
   7.  Language Constructs ......................................... 12
   7.1.  Module and Imports ........................................ 12
   7.2.  Type Definitions .......................................... 12
   7.3.  Function Definitions ...................................... 13
   7.4.  Match Expression .......................................... 14
   7.5.  Bindings and Assignment ................................... 14
   7.6.  Conditionals .............................................. 15
   7.7.  Loop ...................................................... 15
   7.8.  Error Propagation ......................................... 16
   7.9.  Arena Blocks .............................................. 16
   7.10. Complete Example .......................................... 17
   8.  Memory Model ................................................ 18
   8.1.  Arena Allocation .......................................... 18
   8.2.  Static Lifetime ........................................... 18
   8.3.  Memory Safety Guarantees .................................. 19
   9.  Structured Error Protocol ................................... 19
   9.1.  Compiler Diagnostic Schema ................................ 19
   9.2.  Runtime Trap Schema ....................................... 20
   10. Compilation Pipeline ........................................ 21
   11. Hardware Strategy ........................................... 22
   11.1. Capital Investment Case ................................... 22
   11.2. Local Pre/Post Processing ................................. 23
   11.3. Post-Generation Quality Processing ........................ 24
   12. Parallel Differential Testing ............................... 24
   12.1. Validation Properties ..................................... 25
   12.2. Corpus Entry Metadata ..................................... 25
   13. Corpus Generation ........................................... 26
   13.1. Generation Architecture ................................... 26
   13.2. Training Curriculum ....................................... 26
   13.3. Grammar-Based Supplemental Generation ..................... 28
   14. Agent-Based Review Pipeline ................................. 28
   15. Model Training .............................................. 29
   15.1. Base Model Selection ...................................... 29
   15.2. Training Method ........................................... 30
   15.3. Training Data Format ...................................... 30
   15.4. Purpose-Built Tokenizer ................................... 31
   16. Validation Workstreams ...................................... 32
   17. Project Stages .............................................. 34
   17.1. Stage 1 — Falsification ................................... 34
   17.2. Stage 2 — Language Viability .............................. 35
   17.3. Stage 3 — Ecosystem Proof ................................. 35
   17.4. Stage 4 — Standard Pathway ................................ 36
   18. Cost Model .................................................. 36
   19. Self-Improvement Loop ....................................... 39
   20. Benchmark Targets ........................................... 40
   21. Repository Structure ........................................ 41
   22. Milestones .................................................. 42
   23. Open Questions .............................................. 43
   24. Security Considerations ..................................... 44
   25. IANA Considerations ......................................... 45
   26. Risks ....................................................... 45
   27. References .................................................. 46
   27.1. Normative References ...................................... 46
   27.2. Informative References .................................... 46
   Appendix A.  Acknowledgements ................................... 47
   Author's Address ................................................ 47
```

---

## 1.  Introduction

### 1.1.  Motivation

   Large language models generating code in conventional programming
   languages face a structural inefficiency: those languages were
   designed for human authors.  Their syntax includes optional delimiters,
   multiple equivalent forms for the same construct, whitespace
   significance, comment syntax, and verbose keyword choices.  These
   properties aid human comprehension but impose token overhead, increase
   ambiguity in generation, and produce error messages formatted for
   terminal display rather than machine consumption.

   The generate-compile-repair loop — in which an LLM generates source,
   a compiler validates it, errors are returned, and the model corrects
   them — is the dominant pattern for reliable LLM code generation.  The
   efficiency of this loop depends on three properties:

   a)  Token count per generation pass.  Fewer tokens per correct program
       means lower cost and higher throughput.

   b)  First-pass compile success rate.  An unambiguous, constrained
       syntax reduces the generation space of invalid programs.

   c)  Diagnostic machine-readability.  Structured, schema-stable error
       output allows mechanical fix application before model invocation,
       reducing round trips.

   toke addresses all three properties by designing the language around
   the machine as the primary author and consumer rather than as a
   secondary user of a human-oriented tool.

### 1.2.  Core Thesis

   The central claim of this proposal is:

      A sufficiently constrained, unambiguous, token-efficient language
      WILL reduce end-to-end LLM code generation cost — measured as
      (tokens x iterations x error rate) — by a material margin
      sufficient to justify building and training a purpose-native model.

   This thesis is treated as an empirical hypothesis to be validated
   before full investment.  A four-phase project plan with explicit
   go/no-go gates is defined in Section 17.  Gate 1 at Month 8 is the
   primary validation point.  If the thesis does not hold at Gate 1,
   the project MUST pivot to an IR-level approach without proceeding
   to full language development.

### 1.3.  Scope of This Document

   This document covers:

   o  Language identity, name rationale, and namespace analysis
   o  Design principles
   o  Character set specification and development profile history
   o  Symbol assignment and keyword table
   o  Core language constructs with examples
   o  Memory model
   o  Structured error protocol and diagnostic schema
   o  Compilation pipeline
   o  Hardware strategy for corpus generation and model training
   o  Parallel differential testing methodology
   o  Corpus generation curriculum
   o  Agent-based review pipeline
   o  Model training configuration and data format
   o  Seven validation workstreams
   o  Four-stage project plan with go/no-go gates
   o  Cost model
   o  Self-improvement loop
   o  Benchmark targets
   o  Repository structure and milestones
   o  Open questions and risks

   This document does not fully specify:

   o  Concurrency semantics (deferred)
   o  Full foreign function interface rules (deferred)
   o  Package registry governance (deferred)
   o  Formal memory model with ownership annotations (deferred)
   o  Debugger metadata formats (deferred)
   o  Canonical binary intermediate representation (deferred)
   o  Purpose-built tokenizer vocabulary (deferred pending corpus
      completion)

---

## 2.  Conventions and Terminology

   The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
   "SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY", and
   "OPTIONAL" in this document are to be interpreted as described in
   BCP 14 [RFC2119] [RFC8174] when, and only when, they appear in all
   capitals, as shown here.

   The following terms are used throughout this document:

   toke:
      The programming language defined by this document.  Spoken as
      "toke" (rhymes with "spoke").

   tk:
      The written shorthand for toke.  Used in file extensions (.tk),
      compiler binary names (tkc), and source-level module paths.

   tkc:
      The reference compiler binary for toke.

   Development profile:
      The 80-character bootstrapping variant of the toke source
      language, used during early compiler development and corpus
      generation to maintain compatibility with existing LLM
      tokenizers.  Uses uppercase keywords (F=, T=, I=, M=) and
      bracket array syntax ([a;b;c]).  Not the normative language.

   Production profile (normative):
      The 56-character variant that constitutes the toke language as
      specified in this document.  Uses sigil-based encoding ($type,
      @(a;b;c)) and lowercase keywords (f=, t=, i=, m=).  Designed
      for use with the purpose-built toke tokenizer.

   LLM:
      Large Language Model.  An autoregressive neural language model
      used as the primary code generator in the toke workflow.

   Repair loop:
      An automated cycle of: generate source, compile, extract
      structured diagnostic, apply fix, recompile.  Successful loops
      terminate in a validated binary without human intervention.

   Arena:
      A lexically scoped memory region.  All allocations within an
      arena are freed in bulk when the arena's scope exits.

   Corpus:
      The collection of validated (task description, toke source) pairs
      used to fine-tune LLMs on the toke language.

   Diagnostic:
      A machine-readable structured record emitted by the compiler
      describing a compilation error, warning, or informational message.

   Pass@k:
      The probability that at least one of k independently generated
      programs for a given task passes all correctness tests.

   BPE:
      Byte-Pair Encoding.  The tokenization algorithm used by most
      current LLMs for vocabulary construction.

---

## 3.  Language Identity

### 3.1.  Name and Shorthand

   The language is named **toke**, spoken as a single syllable.  The
   written shorthand is **tk**.  This relationship parallels the Go
   language: the language is named Go, the files use the .go extension,
   and tooling uses the go binary name.  For toke: the language is toke,
   the files use .tk, and the compiler binary is tkc.

   The name is intentionally self-referential.  A language designed to
   minimise token usage has a name that is itself a single token in the
   cl100k_base vocabulary used by current-generation LLMs.  "toke" is a
   contraction of "token", embedding the language's compression principle
   in its own identifier.

   The package registry name is **tokelang** across all package
   ecosystems (npm, crates.io, PyPI, Homebrew) to avoid collision with
   short names already in use.

### 3.2.  Namespace Conflict Analysis

   The following potential conflicts were assessed:

   Tcl/Tk (1991):
      The Tcl/Tk GUI toolkit uses "Tk" with a capital T.  It is always
      written "Tk" or "Tcl/Tk" in documentation, invoked as
      "package require Tk" in Tcl source, and operates in a distinct
      domain (GUI toolkit vs compiled systems language).  No conflict.

   toke-runner (crates.io):
      A small hobby project providing a TOML-based command runner.
      Installed as "toke-runner", not "toke".  No trademark.  The
      tokelang package name sidesteps this completely.

   tokei (Rust):
      A code statistics tool.  Different spelling, different domain.
      Phonetically adjacent but contextually distinct.

   .tk TLD:
      Tokelau's country code top-level domain.  The canonical domain
      for toke is tokelang.dev, not toke.tk or tk.io.

   No registered trademark exists on "toke" as software.  Registration
   of "tokelang" across all package namespaces is RECOMMENDED as an
   immediate first step before any public announcement.

---

## 4.  Design Principles

   All toke implementations SHALL adhere to the following design
   principles.  Where any implementation decision conflicts with a
   principle, the principle takes precedence over convenience.

### 4.1.  Machine-First Syntax

   The language SHALL define exactly one canonical syntactic form for
   each construct.  Synonym constructs, optional delimiters, and
   style-variant spellings are prohibited.

### 4.2.  Deterministic Structure

   A valid toke source unit SHALL parse to exactly one unambiguous
   syntax tree under the normative grammar.  The grammar is LL(1):
   the parser SHALL NOT require more than one token of lookahead.

### 4.3.  Token Efficiency

   The source language SHALL minimise unnecessary verbosity, redundant
   delimiters, whitespace-as-syntax, and optional surface variation.
   Every character in the character set MUST be necessary.  Every token
   in generated output MUST carry semantic information.

### 4.4.  Strong Explicit Typing

   All values, interfaces, and function signatures SHALL have explicitly
   stated types.  Implicit type inference is not defined.

### 4.5.  Structured Failure

   All compiler and runtime outputs SHALL be machine-readable and
   schema-stable.  Free-form prose error messages SHALL NOT be the
   primary diagnostic channel.  Every error condition SHALL carry a
   stable error code, a machine-parseable location, and a suggested
   fix where that fix is mechanically derivable.

### 4.6.  Incremental Compilability

   A single source file SHALL be independently parseable and type-
   checkable given only its declared imports and the type interfaces
   those imports expose.

### 4.7.  No Hidden Behaviour

   The language SHALL NOT define implicit conversions, context-sensitive
   semantic changes, or undefined behaviour in well-typed programs.
   Every operation has a defined result or a defined trap behaviour.

### 4.8.  Native Execution Target

   The language SHALL support native compilation producing self-
   contained binaries for x86-64 and ARM64.  No runtime interpreter,
   virtual machine, or garbage collector SHALL be required in the
   distributed binary.

### 4.9.  Arena Memory Discipline

   Memory allocation SHALL follow a lexical arena discipline.  All heap
   allocations within a function body or explicit arena block are freed
   deterministically on scope exit.

---

## 5.  Character Set Specification

   toke uses exactly 56 structural characters.  No character outside
   this set SHALL appear in toke source in a structural position.
   Arbitrary UTF-8 is permitted within string literal content.

### 5.1.  The 56-Character Alphabet

```
   CLASS         CHARACTERS                                         COUNT
   -------------------------------------------------------------------
   Lowercase     a-z                                                   26
   Digits        0-9                                                   10
   Symbols       ( ) { } = : . ; + - * / < > ! | " $ @ ^ ~           20
   -------------------------------------------------------------------
   TOTAL                                                               56
```

   The following character classes are explicitly excluded from
   structural positions:

   o  Whitespace (U+0020, U+0009, U+000D, U+000A): structurally
      meaningless.  The semicolon (;) serves as the universal
      separator.  Two programs that differ only in whitespace between
      tokens are lexically identical.

   o  Comment delimiters: no comment syntax is defined.  Documentation
      and metadata are stored outside source files.

   o  Uppercase letters (A-Z): eliminated through sigil-based encoding.

   o  Square brackets [ ]: replaced by @() for arrays and .get() for
      indexing.

   o  Excluded symbols: #, %, &, backtick, backslash, single-quote,
      comma, question-mark.

   The characters ^ and ~ are reserved and unassigned.  They MUST NOT
   be used in toke source.  They are held for future language extension
   without requiring a profile version change.

   Encoding conventions:

   Type sigil ($):
      All type names are prefixed with $ and written in lowercase.
      Example: $user, $str, $err.  The $ character precedes a type
      name in 100% of its occurrences, forming highly predictable
      co-occurrence patterns under BPE tokenization.

   Array sigil (@):
      Array literals use @(a;b;c) syntax.  Array indexing uses
      a.get(n) for variable indices.  The @ character precedes ( in
      100% of its occurrences.  BPE training absorbs these into single
      merged tokens: $user, $str, @( each become one vocabulary entry
      after sufficient corpus exposure.

### 5.2.  Development Profile (80 Characters) — Historical

   During early development, toke used an 80-character bootstrapping
   profile to maintain compatibility with existing LLM tokenizers
   (cl100k_base).  This profile used uppercase keywords (F=, T=, I=,
   M=), uppercase-initial type names (User, Str), and bracket array
   syntax ([a;b;c], a[n]).

   The development profile served three purposes:

   1.  Bootstrapping the compiler and conformance suite without
       requiring a purpose-built tokenizer.
   2.  Generating the initial training corpus using existing LLMs
       that had no prior exposure to toke syntax.
   3.  Measuring baseline token efficiency against conventional
       languages before applying sigil-based compression.

   The development profile is NOT the normative toke language.
   Mechanical translation between the development profile and the
   production 56-character language is fully deterministic and
   implemented in the reference compiler via --profile1 and --profile2
   flags.

   The 80-character profile uses 26 lowercase + 26 uppercase + 10
   digits + 19 symbols = 81 structural characters.  It is documented
   here for methodological transparency and to contextualise the
   Gate 1 benchmark results, which were measured using the development
   profile corpus.

### 5.3.  Token Count Estimates

   Illustrative token counts for a typical HTTP handler across profiles
   and tokenizers.

```
   Configuration                          Estimated Tokens
   --------------------------------------------------------
   tk (development profile), cl100k_base  ~38
   tk (production), cl100k_base            ~43  (+5, sigil overhead)
   tk (dev profile), purpose-built        ~26  (common patterns merge)
   tk (production), purpose-built         ~22  (sigils merge)

   Python (benchmark baseline)            ~85
   TypeScript (benchmark baseline)        ~92
   --------------------------------------------------------
```

   The toke production profile with purpose-built tokenizer is
   projected to achieve approximately 4x token density versus the
   Python baseline for equivalent logic.  Token efficiency values
   were validated at Gate 1 (2026-04-03): 12.5% token reduction vs
   cl100k_base confirmed, 3x density vs Python confirmed.

---

## 6.  Symbol Assignment

   Every symbol in the toke character set has exactly one normative
   role.  Where a symbol serves two roles, the disambiguation is
   determined by syntactic position, not by context-sensitive rules.

### 6.1.  Operator and Delimiter Table

```
   Symbol  Primary role              Secondary role
   ------  ------------------------  --------------------------
   =       Bind / define             Equality test in match
   :       Type annotation           Field separator in struct
   .       Member access             Module path separator
   ;       Statement terminator      Argument / field separator
   (       Call open / group open    (none)
   )       Call close / group close  (none)
   {       Block open                Struct literal open
   }       Block close               Struct literal close
   [       (dev profile only)        (dev profile only)
   ]       (dev profile only)        (dev profile only)
   +       Add                       String concatenation
   -       Subtract                  Unary negation
   *       Multiply                  Pointer deref (FFI only)
   /       Divide                    (none)
   <       Return (short form)       Less-than comparison
   >       Greater-than comparison   (none)
   !       Error propagation         Logical not
   |       Match block open          Union type separator
   "       String literal delimiter  (none)
```

   Sigil symbols:

```
   Symbol  Role
   ------  ----------------------------------------
   $       Type name sigil prefix
   @       Array literal sigil: @(a;b;c)
   ^       Reserved, unassigned
   ~       Reserved, unassigned
```

### 6.2.  Keywords

   Twelve identifiers are reserved as keywords.  They MUST NOT be used
   as user-defined identifiers.

```
   Keyword  Role
   -------  ------------------------------------------
   f        Function definition
   t        Type definition
   i        Import declaration
   m        Module declaration
   if       Conditional branch
   el       Else branch (follows closing } of if only)
   lp       Loop (the single loop construct)
   br       Break — exits the innermost lp block
   let      Immutable binding
   mut      Mutable qualifier on binding
   as       Explicit type cast
   rt       Return (long form; equivalent to <)
```

   All keywords are lowercase.  The declaration keywords f, t, i, and m
   are single characters.  Boolean literals true and false are
   predefined identifiers, not keywords, and MUST NOT be redefined.

   NOTE: In the development profile (80-character), the declaration
   keywords are uppercase (F, T, I, M).  The production language uses
   lowercase throughout.

---

## 7.  Language Constructs

### 7.1.  Module and Imports

   Every source file begins with exactly one module declaration,
   followed by zero or more import declarations, followed by type,
   constant, and function declarations in that order.

   Module declaration syntax:

```
   m=module.path;
```

   Import declaration syntax:

```
   i=localAlias:module.path;
```

   The local alias is the name by which the imported module's exports
   are accessed within the file.  Wildcard imports are prohibited.

   Example:

```
   m=api.user;
   i=http:std.http;
   i=db:std.db;
   i=json:std.json;
```

   Following these declarations, http.$req, db.one, and json.enc are
   accessible by their aliased paths.

### 7.2.  Type Definitions

   Syntax:

```
   t=$typename{field1:$type1;field2:$type2};
```

   A type declaration defines either a struct type or a sum type
   (tagged union).  The distinction is lexical:

   o  Struct: all field names are lowercase identifiers.
   o  Sum type: all variant names begin with the $ sigil.
   o  Mixing both conventions in one type is a compile error (E2010).

   Struct example:

```
   t=$user{id:u64;name:$str;email:$str};
```

   Sum type example:

```
   t=$user_err{
     $not_found:u64;
     $bad_input:$str;
     $db_err:$str
   };
```

   In a sum type, each variant name is a tag and its type is the
   payload carried by that variant.

### 7.3.  Function Definitions

   Syntax:

```
   f=name(param1:$type1;param2:$type2):$return_type!$error_type{
     body
   };
```

   Every function declaration MUST explicitly state all parameter
   names, parameter types, return type, and the error type if the
   function is fallible.

   A function without !$error_type is total: it MUST NOT contain error-
   propagation operations (compile error E3001).

   A function with !$error_type is partial: all error-propagation
   operations in the body MUST be coercible to $error_type.

   Total function example:

```
   f=add(a:i64;b:i64):i64{
     <a+b
   };
```

   Partial function example:

```
   f=get_user(id:u64):$user!$user_err{
     r=db.one("SELECT id,name FROM users WHERE id=?";@(id))
       !$user_err.$db_err;
     <$user{id:r.u64(id);name:r.str(name)}
   };
```

### 7.4.  Match Expression

   Syntax:

```
   expr|{
     Variant1:binding1 result_expr;
     Variant2:binding2 result_expr
   }
```

   Match is exhaustive.  The compiler MUST reject any match that does
   not cover all variants of the matched type (compile error E4010).
   All match arms MUST return the same type (compile error E4011).

   Match is an expression, not a statement.  Its result type is the
   common type of all arm expressions.

   Example:

```
   get_user(id)|{
     $ok:u   <$res.ok(json.enc(u));
     $err:e  <$res.err(json.enc(e))
   }
```

### 7.5.  Bindings and Assignment

   Immutable binding:

```
   let name=expr;
```

   After declaration, name MUST NOT be reassigned (compile error
   E3010).

   Mutable binding:

```
   let name=mut.initial_value;
```

   The mut. qualifier marks the binding as mutable.  An initial value
   is required.  Subsequent assignment uses bare assignment syntax:

```
   name=new_value;
```

   Assigning to an immutable binding is compile error E3010.
   Assigning to an undeclared name is compile error E3011.

### 7.6.  Conditionals

   Single-branch form:

```
   if(condition){
     body
   }
```

   Two-branch form:

```
   if(condition){
     true_body
   }el{
     false_body
   }
```

   The condition MUST be of type bool.  No implicit truthiness
   conversion is performed (compile error E4001 for non-bool
   condition).  The el keyword is not standalone; it is valid only
   immediately after the closing } of an if block.

### 7.7.  Loop

   toke defines exactly one loop construct:

```
   lp(init_stmt;condition;step_stmt){
     body
   }
```

   Where:

   o  init_stmt: a binding or assignment executed once before the loop.
   o  condition: a bool expression evaluated before each iteration.
   o  step_stmt: a statement executed after each iteration body.

   The br keyword exits the innermost lp immediately.  There is no
   while, do-while, for-each, or until construct.  Recursion is the
   idiomatic alternative for functional patterns.

   Example:

```
   f=sum(arr:@i64):i64{
     let acc=mut.0;
     lp(let i=0;i<arr.len;i=i+1){
       acc=acc+arr.get(i)
     };
     <acc
   };
```

### 7.8.  Error Propagation

   Syntax:

```
   expr!ErrorVariant
```

   The ! operator propagates an error from a partial call.

   o  If expr evaluates to Err(e): the current function returns
      Err(ErrorVariant(e)).
   o  If expr evaluates to Ok(v): execution continues with value v.

   The right-hand side of ! MUST be a variant constructor of the
   current function's declared error type (compile error E3020).

   Example:

```
   f=handle(req:http.$req):http.$res!$api_err{
     body=json.dec(req.body)!$api_err.$bad_request;
     user=db.get_user(body.id)!$api_err.$db_error;
     <http.$res.ok(json.enc(user))
   };
```

### 7.9.  Arena Blocks

   Syntax:

```
   {arena
     stmts
   }
```

   An arena block creates a lexically scoped allocation region.  All
   heap allocations made within the block are freed when the block
   exits, regardless of exit path.  Returning a value that references
   an arena-local allocation across the arena boundary is a compile
   error (E5001).

### 7.10.  Complete Example

   HTTP user API module:

```
   m=api.user;
   i=http:std.http;
   i=db:std.db;
   i=json:std.json;

   t=$user{id:u64;name:$str;email:$str};
   t=$user_err{$not_found:u64;$db_err:$str};

   f=fetch(id:u64):$user!$user_err{
     r=db.one(
       "SELECT id,name,email FROM users WHERE id=?";
       @(id)
     )!$user_err.$db_err;
     <$user{
       id:r.u64(id);
       name:r.str(name);
       email:r.str(email)
     }
   };

   http.get("/users/:id";f=handle(req:http.$req):http.$res{
     id=req.param(id).u64|{
       <http.$res.bad("id must be number")
     };
     fetch(id)|{
       $ok:u  <http.$res.ok(json.enc(u));
       $err:e <http.$res.err(json.enc(e))
     }
   });
```

   This ~300-character module defines types, a database access
   function, and an HTTP route handler.  The route pattern, handler
   signature, database query, type construction, and error handling
   are all verified at compile time.  No runtime surprises arise from
   unhandled error paths.

---

## 8.  Memory Model

### 8.1.  Arena Allocation

   toke uses lexical arena allocation.  Every function body is an
   implicit arena.  All allocations within the function body —
   strings, arrays, structs — are freed when the function returns,
   via any return path including error propagation.

   Explicit {arena ...} blocks create sub-arenas with shorter
   lifetimes.  These are freed when the block exits.

   The allocator is a bump allocator per arena, providing O(1)
   allocation cost.  Deallocation is bulk: the entire arena is freed
   in a single operation on scope exit, with no per-object overhead.

   Allocation rules:

   1.  Allocations within a function body are valid for the lifetime
       of that function call.
   2.  Allocations within an arena block are valid for the lifetime
       of that block.
   3.  A value allocated in inner scope A MUST NOT be returned from,
       passed out of, or stored in a location with a lifetime longer
       than A (compile error E5001).
   4.  Module-level constants and static data are allocated at program
       start and freed at program exit.

### 8.2.  Static Lifetime

   Module-level declarations — constants, connection pools, global
   configuration — have static lifetime.  They are initialised once at
   program start in declaration order and are never freed during
   execution.  Circular initialisation dependencies are a compile
   error (E2020).

### 8.3.  Memory Safety Guarantees

   In well-typed toke code without FFI:

   o  No use-after-free: arena discipline prevents access to freed
      memory.
   o  No buffer overflow: array bounds are checked at runtime (trap
      RT001).
   o  No uninitialised reads: all bindings require initialisation at
      declaration.
   o  No null pointer dereference: toke has no null values.
   o  No data races: concurrency is a deferred feature.

---

## 9.  Structured Error Protocol

   All compiler errors, warnings, and runtime traps are emitted as
   machine-readable structured records.  Human-readable text is
   provided as a secondary field only.  The schema is versioned and
   stable.

### 9.1.  Compiler Diagnostic Schema

   Every diagnostic record SHALL conform to the following schema:

```json
   {
     "schema_version": "1.0",
     "diagnostic_id":  "<string: unique ID for this instance>",
     "error_code":     "<string: stable code, e.g. E4020>",
     "severity":       "<error|warning|info>",
     "phase":          "<lex|parse|import_resolution|name_resolution
                         |type_check|arena_check|ir_lower|codegen>",
     "message":        "<string: human-readable summary>",
     "file":           "<string: source file path>",
     "pos":            "<integer: byte offset from start of file>",
     "line":           "<integer: 1-based line number>",
     "column":         "<integer: 1-based column number>",
     "span_start":     "<integer: byte offset of error start>",
     "span_end":       "<integer: byte offset of error end>",
     "context":        ["<string: source lines surrounding error>"],
     "expected":       "<string: what was expected>",
     "got":            "<string: what was observed>",
     "fix":            "<string: suggested correction if derivable>"
   }
```

   The fix field SHALL be populated whenever the correction is
   deterministic.  Examples of mechanically derivable fixes:

   o  Wrong type accessor: r.str(id) when field is u64.
      fix: "r.u64(id)"

   o  Missing error propagation on a partial call.
      fix: "add !ErrorVariant"

   o  Misspelled variant name.
      fix: "CorrectVariantName"

   o  Missing import for a resolvable stdlib identifier.
      fix: "add i=alias:std.module"

   Diagnostic stability contract:

   o  error_code values are stable across patch versions.
   o  Minor versions MAY add new codes but MUST NOT change existing
      meanings.
   o  fix field values are stable within a compiler version.
   o  Automated repair systems MAY depend on error_code and fix
      stability between patch releases of the same minor version.

   Example diagnostic:

```json
   {
     "schema_version": "1.0",
     "diagnostic_id": "diag-004819",
     "error_code": "E4020",
     "severity": "error",
     "phase": "type_check",
     "message": "field type mismatch in struct initialiser",
     "file": "api/user.tk",
     "pos": 247,
     "line": 12,
     "column": 8,
     "span_start": 242,
     "span_end": 258,
     "context": [
       "10: f=get_user(id:u64):$user!$user_err{",
       "11:   r=db.one(sql;@(id))!$user_err.$db_err;",
       "12:   <$user{id:r.str(id);name:r.str(name)}"
     ],
     "expected": "u64",
     "got": "$str",
     "fix": "r.u64(id)"
   }
```

### 9.2.  Runtime Trap Schema

   Runtime traps emit the following record to stderr and exit with
   code 2:

```json
   {
     "schema_version": "1.0",
     "trap_code": "<string: RT001-RT007>",
     "message": "<string>",
     "file": "<string>",
     "line": "<integer>",
     "function": "<string>",
     "arena_depth": "<integer>",
     "context": ["<string: call stack frames>"]
   }
```

   Defined trap codes:

```
   Code   Condition
   -----  ----------------------------------------
   RT001  Array out-of-bounds access
   RT002  Integer division by zero
   RT003  Integer overflow (checked arithmetic)
   RT004  Null pointer dereference (FFI only)
   RT005  Arena boundary violation
   RT006  Stack overflow
   RT007  Assertion failure
```

---

## 10.  Compilation Pipeline

   A conforming tkc implementation SHALL execute the following phases
   in order:

```
   Source (.tk files)
         |
         v
   Lexer               Flat token stream.  No whitespace tokens.
         |             Each token unambiguously typed from its first
         v             character.

   Parser              AST.  Structured error on grammar violation.
         |             LL(1): one token of lookahead maximum.
         v

   Import resolver     Resolves all imports to interface files.
         |             Fails fast on missing module with available
         v             module list in diagnostic.

   Name resolution     Resolves all identifier references.
         |             Fails with E3011 (unresolved) or E3012
         v             (ambiguous).

   Type checker        Enforces all type rules.  Exhaustiveness,
         |             coercions, arena validity.  Structured error
         v             with fix suggestion.

   IR lowering         AST to toke IR.  SSA form, explicit types,
         |             no syntactic sugar.
         v

   LLVM IR backend     toke IR to LLVM IR.
         |
         v

   LLVM                Native x86-64 or ARM64 binary.
         |
         v

   Binary              No runtime dependency.  ~40KB overhead.
                       Interface (.tki) file emitted alongside.
```

   Compiler frontend: approximately 3,700 lines of C with zero
   external dependencies (excluding LLVM as a build dependency).
   The reference implementation (tkc) achieves this with: lexer
   (~240 lines), parser (~440 lines), name resolution (~610 lines),
   type checker (~600 lines), LLVM IR backend (~990 lines), arena
   allocator (~90 lines), diagnostics (~160 lines), main driver
   (~190 lines).

   Performance targets on reference hardware (Apple M4, single core):

```
   Task                                            Target
   ---------------------------------------------   -------
   Lex + parse a 200-token .tk file                < 2ms
   Full compilation of a 200-token .tk file        < 50ms
   Incremental recompile (interfaces cached)       < 5ms
   Type-check a 50,000-file corpus                 < 60s
```

   These targets ensure the compiler runs synchronously within LLM
   API call timeouts in the repair loop.

---

## 11.  Hardware Strategy

### 11.1.  Capital Investment Case

   The primary hardware platform is a Mac Studio M4 Max with 128GB
   unified memory (USD 7,199).  This is a capital investment, not an
   operating cost.  It serves simultaneously as:

   o  The daily development machine.
   o  The local inference server running Qwen 2.5 Coder 32B at ~25
      tokens/second in 4-bit quantisation, sufficient for corpus
      pre/post-processing at zero API cost.
   o  The corpus validation host running tkc continuously.
   o  The model training platform running QLoRA via Apple's MLX
      framework.
   o  Long-term storage for the full corpus (~2GB for 500K files).

   Training time estimates on M4 Max 128GB:

```
   Model   Method   Estimated Training Time
   ------  -------  -----------------------
   7B      QLoRA    18-24 hours per run
   32B     QLoRA    48-72 hours per run
```

   Break-even versus cloud A100 rental at USD 1.50/hour:
   approximately 4,800 GPU-hours, or ~18 months of intensive use.
   For a 32-month multi-phase project, ownership dominates rental.

   The 128GB configuration MUST be chosen over the 96GB.  The 34B
   model requires approximately 88GB for QLoRA with a full batch and
   optimizer state.  The 96GB configuration provides insufficient
   headroom; the USD 800 delta is warranted.

### 11.2.  Local Pre/Post Processing with Qwen

   The corpus pipeline uses a two-tier generation strategy to minimise
   expensive API calls:

```
   Task description
         |
         v
   Qwen 2.5 Coder 32B (local, Mac Studio)
     - pre-filter: is this task well-specified?
     - pre-generate: attempt tk code locally
     - validate: does it compile? does it pass tests?
         |
         +-- Yes --> save to corpus (zero API cost)
         |
         +-- No/ambiguous --> Claude Haiku 4.5 (batch API)
                    |
                    v
              Generate / fix
                    |
                    v
              Compile + test (local tkc)
                    |
                    +-- Pass --> save to corpus
                    +-- Fail --> structured error --> retry (max 3)
```

   Expected API escalation rates by corpus phase:

```
   Phase  Description        API Escalation Rate
   -----  -----------------  -------------------
   A      Primitives         ~25%  (Qwen handles ~75% locally)
   B      Data structures    ~40%
   C      System interaction ~60%
   D      Applications       ~80%  (Sonnet 4.6 for multi-module)
```

   This strategy reduces API costs by approximately 50-65% compared
   to API-only generation.

### 11.3.  Post-Generation Quality Processing

   After each corpus entry passes compiler validation, Qwen runs
   post-generation checks locally at zero marginal cost:

   o  Structural review: are module boundaries sensible?
   o  Redundancy check: embedding similarity against existing entries.
   o  Style consistency: does this match established tk idioms?
   o  Test coverage: are edge cases exercised?

   Entries failing these checks are flagged for regeneration or
   discarded.  This replaces the majority of human review in corpus
   Phases A through C.

---

## 12.  Parallel Differential Testing

   Every corpus generation task produces implementations in four
   languages simultaneously.  These implementations serve as mutual
   correctness oracles.  This approach is known as differential
   testing or metamorphic testing.

```python
   async def generate_and_validate(task):
       results = await asyncio.gather(
           generate(task, "tk"),
           generate(task, "python"),
           generate(task, "c"),
           generate(task, "java"),
           generate_test_inputs(task)
       )
       binaries = compile_all(results)
       outputs  = run_all(binaries, results.test_inputs)
       verdict  = majority_vote(outputs)

       match verdict:
           Ok:        corpus.add(task, results.tk, metadata(results))
           TkBug:     correction_loop(task, results.tk, verdict.error)
           OtherBug:  discard_bad_language(results, verdict.outlier)
           Ambiguous: discard_task("task description unclear")
```

### 12.1.  Validation Properties

   Correctness oracle:
      Reference language implementations generate correctly at high
      rates — LLMs have extensive training on them.  A tk program that
      disagrees with all three reference implementations is almost
      certainly incorrect in tk, not in the reference languages.

   Performance benchmark:
      Every corpus entry has four native implementations.  Binary
      size, startup time, execution time, and memory usage across all
      four languages emerge automatically across 50,000+ data points.

   Token density measurement:
      Token counts for all four implementations are recorded per entry,
      providing empirical token density data at corpus scale.

   Ambiguity detection:
      When all four implementations disagree, the task description is
      underspecified.  Such tasks are discarded, keeping the corpus
      free of programs that reward hallucinating solutions to ambiguous
      prompts.

   Training progress curve:
      First-attempt compile success rates for all four languages are
      tracked per training iteration.  tk's rate starts lower and
      rises with each fine-tuning cycle.  This curve is the primary
      quantitative evidence that training is working.

### 12.2.  Corpus Entry Metadata

   Each validated corpus entry stores:

```
   task_id:         unique identifier
   tk_tokens:       token count for tk generation
   baseline_tokens: token count for Python reference implementation
   c_tokens:        token count for C reference implementation
   java_tokens:     token count for Java reference implementation
   tk_ratio:        tk_tokens / baseline_tokens (density metric)
   attempts:        number of correction rounds needed
   error_trace:     [broken_code, error_struct, fixed_code] if any
   perf_ratio:      tk_binary_time / c_binary_time
   binary_size:     tk binary bytes
   phase:           A|B|C|D|E
```

---

## 13.  Corpus Generation

### 13.1.  Generation Architecture

```
   Mac Studio (local)              Claude API (burst capacity)
   ------------------              ---------------------------
   Curriculum task generator       Haiku 4.5 batch (Phases A-C)
   Qwen 2.5 Coder 32B inference    Sonnet 4.6 (Phase D)
   tkc compiler + test runner      Sonnet 4.6 + correction loop
   Qwen judge agent
   Corpus storage (local NVMe)
```

### 13.2.  Training Curriculum

   **Phase A — Primitives (target: 50,000; actual: ~27,000 programs)**

   Single functions.  Tasks generated programmatically from templates.
   Topics include:

   o  Arithmetic across all type combinations
   o  String operations: concat, split, slice, format
   o  Array operations: map, filter, fold, sort
   o  Conditional logic: all boolean expression patterns
   o  Recursive algorithms: factorial, Fibonacci, tree traversal
   o  Error propagation: chains of fallible operations

   **Phase B — Data Structures (target: 30,000; actual: ~9,800 programs)**

   2-4 files.  Type definitions separate from function implementations.
   Topics include:

   o  Linked list, stack, queue implemented with arena allocation
   o  Hash map and set
   o  Binary search tree and heap
   o  Graph representations and traversal
   o  Serialisation and deserialisation patterns

   **Phase C — System Interaction (target: 20,000; actual: 5,000 programs)**

   Interaction with stdlib network, file, and process modules.
   Topics include:

   o  HTTP client and server handlers
   o  TCP socket clients and servers
   o  File read, write, and watch patterns
   o  Process spawn and communicate
   o  Database query patterns (SQL and key-value)

   **Phase D — Applications (target: 5,000; actual: 5,000 programs)**

   Multi-module programs completing a real task.  Agent review applied
   to approximately 20% of entries.  Topics include:

   o  REST API with database backend (5-8 source files)
   o  CLI tool with subcommands and configuration
   o  Background worker with queue processing
   o  Static file server with routing and caching
   o  Proxy and middleware patterns

   **Phase E — Complex Systems (target: 500 programs)**

   Architectural templates requiring coordinated multi-module design.
   All entries receive human architectural review.  Topics include:

   o  Full web application (frontend, API, database, sessions)
   o  Distributed key-value store
   o  Simple shell with pipes and redirection
   o  Build system
   o  Interpreter for a minimal language

   Phase E programs represent the aspirational capability ceiling.
   Each is generated by the model, reviewed and corrected by a human
   if necessary, then added to the training set.

### 13.3.  Grammar-Based Supplemental Generation

   In addition to LLM generation, the corpus is supplemented with:

   o  Random valid AST generation: programmatically sampled valid tk
      programs subject to grammar and type rules, ensuring full
      syntax coverage including constructs LLMs rarely generate.

   o  Transpilation: a subset of Python and C functions converted to
      tk equivalents, providing realistic code shapes with semantics
      verified against the source.

   o  Adversarial injection: deliberately tricky constructs — deep
      nesting, long identifier chains, subtle type mismatches — and
      intentional bugs included as repair training examples.

   o  Deduplication: embedding similarity check against existing
      entries before any addition.  Near-duplicate entries are
      discarded.

---

## 14.  Agent-Based Review Pipeline

   Three review agents run in sequence across all corpus phases.

   **Agent 1 — Compiler Oracle**

   The tkc compiler itself.  Validates syntax, type safety, import
   resolution, exhaustive match coverage, and error propagation
   completeness.  Produces pass/fail with a structured diagnostic.
   Zero cost.  No human involvement.

   **Agent 2 — Local Judge (Qwen 2.5 Coder 32B)**

   Runs on Mac Studio after Agent 1 passes.  Zero marginal cost.
   Evaluates:

   o  Does the program implement the stated task correctly?
   o  Are module boundaries sensible for the domain?
   o  Is error propagation complete across all failure paths?
   o  Is this entry a near-duplicate of an existing entry?
   o  Would the program produce correct output on edge-case inputs?

   Outputs pass, flag, or reject with structured reasoning.

   **Agent 3 — Architectural Review (Claude Sonnet 4.6)**

   Invoked only for Phase D and E entries that passed Agents 1 and 2.
   API cost incurred.  Applied to approximately 20% of Phase D
   entries and approximately 60% of Phase E entries.  Evaluates:

   o  Is the multi-module design coherent?
   o  Are abstraction boundaries correctly placed?
   o  Does the error handling strategy hold across the full program?
   o  Would a senior engineer recognise this as idiomatic?

   **Human Review**

   Reserved for Phase E programs only.  Focused on architectural
   soundness of novel complex systems — the class of judgment that
   automated agents cannot reliably provide for programs with no
   prior examples.  Target: approximately 80 person-hours total
   across the project.

---

## 15.  Model Training

### 15.1.  Base Model Selection

   Primary fine-tune target: **Qwen 2.5 Coder 7B Instruct**

   Selection rationale:

   o  Trained on 5.5 trillion tokens with heavy code weighting.
   o  Achieves 88.4% HumanEval, competitive with models 5-10x larger.
   o  MLX-compatible: fine-tunes directly on Mac Studio via Apple's
      MLX framework with no cloud dependency.
   o  Open weights under a commercial-use-permitted licence.
   o  Sufficient for rapid iteration on Mac Studio hardware.

   Secondary target: **Qwen 2.5 Coder 32B** for the language viability
   phase, accommodated by the 128GB Mac Studio configuration.

### 15.2.  Training Method

   QLoRA (Quantized Low-Rank Adaptation) via Apple MLX.

```
   base_model:     Qwen2.5-Coder-7B-Instruct
   quantization:   4-bit NF4
   lora_rank:      64
   lora_alpha:     128
   lora_dropout:   0.05
   target_modules: all linear layers
   learning_rate:  2e-4 with cosine decay
   batch_size:     8 (gradient accumulation x4 = effective 32)
   epochs:         3
   warmup_steps:   100
   hardware:       Mac Studio M4 Max 128GB, Apple MLX framework
```

### 15.3.  Training Data Format

   Initial model training uses the development profile (80-character)
   corpus, since this is the corpus that was generated and validated
   during the bootstrapping phase.  The training data format uses
   ChatML with a system prompt establishing toke expertise.

   The Gate 1 fine-tune was trained on approximately 73,000 examples
   from the development profile corpus (eval loss: 0.158 after 1
   epoch on Qwen 2.5 Coder 7B Instruct via QLoRA on Apple MLX).

   Direct generation examples (development profile):

```
   [INST] Generate a tk function that fetches a user by ID from
   PostgreSQL.  Available: db.Conn.
   Types: User{id:u64;name:Str}, DbErr{query_err:Str}
   [/INST]
   F=get_user(id:u64):User!DbErr{
     r=db.one("SELECT id,name FROM users WHERE id=?",[id])
       !DbErr.query_err;
     <User{id:r.u64(id);name:r.str(name)}
   }
```

   Production profile training will follow the same format with
   sigil-based syntax once the Gate 1 evaluation is complete:

```
   [INST] Generate a tk function that fetches a user by ID from
   PostgreSQL.  Available: db.$conn.
   Types: $user{id:u64;name:$str}, $db_err{query_err:$str}
   [/INST]
   f=get_user(id:u64):$user!$db_err{
     r=db.one("SELECT id,name FROM users WHERE id=?";@(id))
       !$db_err.query_err;
     <$user{id:r.u64(id);name:r.str(name)}
   }
```

   Correction examples (highest training value):

```
   [INST] Fix this tk program.
   Error: stage:type_check;error:field_type_mismatch;
   pos:89;expected:u64;got:$str;fix:r.u64(id)
   [BROKEN]
   f=get_user(id:u64):$user!$db_err{
     r=db.one(sql;@(id))!$db_err.query_err;
     <$user{id:r.str(id);name:r.str(name)}
   }
   [/INST]
   f=get_user(id:u64):$user!$db_err{
     r=db.one(sql;@(id))!$db_err.query_err;
     <$user{id:r.u64(id);name:r.str(name)}
   }
```

   Multi-language comparison examples (tokenizer training):

```
   [TASK] Sum all integers in an array.
   [PYTHON 85t] def sum_arr(a): return sum(a)
   [C 62t] int sum_arr(int*a,int n){int s=0;
           for(int i=0;i<n;i++)s+=a[i];return s;}
   [TK 28t] f=sum_arr(a:@i64):i64{
              let s=mut.0;
              lp(let i=0;i<a.len;i=i+1){s=s+a.get(i)};
              <s}
   [RATIO] 0.33x baseline tokens.
   [STATUS] All four implementations agree on 1,000 test inputs.
```

### 15.4.  Purpose-Built Tokenizer

   The purpose-built tokenizer is trained on the validated corpus,
   mechanically translated to the production 56-character profile.

```
   Vocabulary size:   32,768 tokens (2^15)
   Algorithm:         Byte-Pair Encoding (BPE)
   Training corpus:   validated .tk files translated to production profile
```

   Training process:

   1.  Collect all validated .tk corpus files.
   2.  Mechanically translate from development profile to production
       profile (uppercase → $sigil, [] → @(), etc.).
   3.  Strip string literal contents (tokenize separately as natural
       language to avoid interference).
   4.  Run BPE training targeting a vocabulary of 32,768.
   5.  Verify that the 100 most common tk constructs are single
       tokens.
   6.  Manually inspect and correct pathological merges.
   7.  Freeze vocabulary.

   Expected token density improvement: 2.5-4x fewer LLM tokens per
   tk program compared to cl100k_base, arising from the highly
   repetitive and structurally constrained nature of toke source.

---

## 16.  Validation Workstreams

   Seven workstreams run in parallel with development throughout the
   project.  Workstream findings feed directly into go/no-go gate
   decisions.

   **WS1 — Representation Validity**

   Core question: Does tk genuinely reduce token count and improve
   generation quality compared to evaluation baselines?

   Metrics: token count per task, Pass@1, repair iterations,
   end-to-end generation cost.

   Go/no-go criterion: if tk's end-to-end cost is not materially
   better than the evaluation baseline, the language thesis is weak
   and the project MUST pivot to an IR-level approach.

   **WS2 — Semantic Design Adequacy**

   Core question: Can tk express real-world needs — concurrency,
   generics, FFI, versioned modules — without undermining generation
   efficiency?

   Tests: generic container types, async task model, C FFI, module
   versioning.  Success criterion: LLMs can generate correct code for
   these constructs and they remain token-compact.

   **WS3 — Compiler Formalism**

   Core question: Is the grammar fully specified, unambiguous, and
   formally complete?

   Deliverable: EBNF grammar validated by parser generator (zero
   ambiguity warnings), formal type rules, memory model specification,
   100% conformance test coverage of all language features.

   **WS4 — Corpus Generation Science**

   Core question: Is the training corpus high-quality, non-
   contaminated, and sufficient for model generalisation?

   Methods: grammar-based generation, transpilation, adversarial
   injection, deduplication, strict holdout separation.

   Success criterion: greater than 80% of benchmark tasks solvable
   within 3 iterations by the trained model on held-out test sets.

   **WS5 — Benchmark and Evaluation Design**

   Core question: Are the benchmarks rigorous, with hidden tests that
   catch semantic errors rather than surface correctness?

   Benchmark categories:

   o  D2C (Algorithmic): single-file computational tasks.
   o  C2C (System-level): multi-file project tasks.
   o  M2C (Maintenance): bug-fix and refactor tasks.
   o  I2C (Interop): cross-language integration tasks.

   Metrics: pass@k, mean repair iterations, adversarial pass rate.

   **WS6 — Ecosystem and Tooling**

   Core question: Can tk be used in practice without custom
   infrastructure for every consumer?

   Deliverables: language server protocol stub, package manifest
   format, IDE syntax highlighting, error-driven repair API, CI
   integration templates.

   **WS7 — Standardisation Pathway**

   Core question: What does a standardisable, interoperable tk
   look like, and what governance structure enables it?

   Deliverables: AST/IR exchange format specification, formal error
   schema, model-compiler protocol specification, package registry
   prototype, draft governance document for open consortium.

---

## 17.  Project Stages

   NOTE: The project phases below (Stage 1-4) describe the project
   timeline and validation gates.  These are distinct from the
   language profiles: the "development profile" (80-char) and
   "production profile" (56-char) described in Section 5.

### 17.1.  Stage 1 — Falsification (Months 1-8)

   Objective: test the core thesis with minimal implementation.  If tk
   shows no material advantage, the project stops here.

   Deliverables:

   o  Minimal compiler: functions, records, imports, errors.
   o  ~47,000 corpus programs across Phases A-D, differential-tested.
   o  Multi-language benchmark suite with 500 hidden test tasks.
   o  Token efficiency measurement report.
   o  Pass@1 measurements across all languages.
   o  First fine-tuned model (7B QLoRA on development profile corpus).

   Status (as of April 2026):

   o  Compiler: 3,700 lines of C, 90/90 conformance tests, 9/9 e2e
      tests.  LLVM IR backend producing native binaries for x86-64
      and ARM64.
   o  Corpus: ~47,000 programs generated (27K Phase A, 10K Phase B,
      5K Phase C, 5K Phase D).
   o  Training: QLoRA fine-tune of Qwen 2.5 Coder 7B Instruct
      completed (eval loss 0.158, 73K training examples, 1 epoch).
   o  Benchmark: 1,000 held-out test tasks (120 inputs each).
      Gate 1 result: 63.7% Pass@1 (588/923 compilable tasks).

   Go/No-Go Gate 1 (Month 8):
      tk shows greater than 10% token reduction AND equal or better
      Pass@1 on held-out D2C tasks.  Failure: halt language
      development and pivot to typed-IR approach without full
      language build.

   Key costs:
   o  Mac Studio purchase: USD 7,199 (capital)
   o  Corpus API costs: ~USD 85 (Haiku batch + prompt caching)
   o  First model training: local (Mac Studio MLX, zero cloud cost)

### 17.2.  Stage 2 — Language Viability (Months 8-14)

   Objective: transition to the production 56-character profile, extend
   tk with essential features, and validate expressiveness without
   sacrificing the token efficiency gains.

   Deliverables:

   o  Extended compiler: production profile support, generics, async
      model, C FFI, module versioning.
   o  Corpus translated to production profile, extended with Phase B
      and C programs.
   o  Purpose-built tokenizer, trained on production profile corpus.
   o  Production profile fine-tuned model: Qwen 2.5 Coder 7B.
   o  Benchmark results for system-level tasks.

   Go/No-Go Gate 2 (Month 14):
      tk with full features retains token efficiency advantage AND the
      trained 7B model outperforms general models on tk-specific tasks
      by a measurable margin.  Failure: redesign extended feature set.

### 17.3.  Stage 3 — Ecosystem Proof (Months 14-26)

   Objective: build the end-to-end ecosystem.  Multiple models, full
   benchmark suite, working toolchain, autonomous improvement loop.

   Deliverables:

   o  Production-quality compiler with full stdlib.
   o  Full corpus Phases A through E (~105,500 programs).
   o  Fine-tuned models: 7B and 32B on the full corpus.
   o  Language server stub and package registry prototype.
   o  Self-improvement loop running autonomously.
   o  Multi-model evaluation across Qwen and Llama families.

   Go/No-Go Gate 3 (Month 26):
      Multiple LLM families achieve greater than 70% Pass@1 on held-
      out Phase C tasks AND the self-improvement loop demonstrably
      improves corpus quality over successive iterations.  Failure:
      re-evaluate standard prospects.

### 17.4.  Stage 4 — Standard Pathway (Months 26-32)

   Objective: formalise into a publishable standard.  Propose to open
   consortium.

   Deliverables:

   o  Formal language specification (grammar + semantics, EBNF).
   o  Published conformance suite.
   o  Standard error schema and AST exchange format.
   o  Package registry initial public release.
   o  Proposal document for open consortium or standards body.
   o  Self-redesign pilot: model proposes language construct revisions
      based on error pattern analysis.

   Go/No-Go Gate 4 (Month 32):
      All benchmark evidence meets thresholds; formal specification
      complete; conformance suite published; consortium proposal
      ready.  Failure: distill findings into a typed-IR standard.

---

## 18.  Cost Model

   18.1.  Capital Investment

```
   Item                              Cost      Notes
   --------------------------------  --------  --------------------------
   Mac Studio M4 Max 128GB / 8TB     USD 7,199  Project compute platform
```

   The Mac Studio is not an operating cost.  It is the project's
   compute platform for the full 32-month duration.

   18.2.  Stage 1 — Falsification

```
   Item                          Detail                    Cost
   ----------------------------  ------------------------  ------
   Compiler build                ~20hrs Claude Code        USD 40
   Test harness and tooling      ~10hrs Claude Code        USD 20
   Phase A corpus generation     50K programs, Haiku batch USD 60
   Parallel generation           Same tasks, 3 languages   USD 25
   Benchmark suite               ~15hrs Claude Code        USD 30
   API infrastructure setup                                USD 25
   ----------------------------  ------------------------  ------
   Stage 1 operating total                                  USD 200
   Mac Studio pro-rata (6mo)     $7,199 / 36mo x 6         USD 1,200
   Stage 1 total                                           USD 1,400
```

   18.3.  Stage 2 — Language Viability

```
   Item                          Detail                    Cost
   ----------------------------  ------------------------  ------
   Extended compiler             ~80hrs Claude Code        USD 160
   Phase B-C corpus              50K programs, mixed API   USD 280
   Parallel generation           Languages + test inputs   USD 120
   Agent 3 review, Phase D       ~20% of 5K entries        USD 80
   Tokenizer training            Local CPU job             USD 0
   7B QLoRA fine-tune (3 runs)   Mac Studio, MLX           USD 0
   32B QLoRA fine-tune (2 runs)  Mac Studio, MLX           USD 0
   Cloud A100 burst              40hrs Lambda Labs         USD 60
   Evaluation and iteration                                USD 80
   Infrastructure                                          USD 120
   Human review Phase D          ~40hrs at $25/hr          USD 1,000
   Mac Studio pro-rata (8mo)                               USD 1,600
   Stage 2 total                                           USD 3,500
```

   18.4.  Stage 3 — Ecosystem Proof

```
   Item                          Detail                    Cost
   ----------------------------  ------------------------  ------
   Full compiler and stdlib       ~200hrs Claude Code       USD 400
   Phase D-E corpus               5,500 programs            USD 320
   Phase E human review           ~80hrs expert review      USD 2,000
   DPO preference data (human)    ~100hrs labelling         USD 2,500
   7B fine-tune iterations        Mac Studio                USD 0
   32B fine-tune iterations       Mac Studio + cloud burst  USD 120
   Cloud burst                    100hrs A100               USD 150
   Evaluation and benchmarking                              USD 200
   Language server and tooling    ~40hrs Claude Code        USD 80
   Infrastructure (12mo)                                    USD 300
   Mac Studio pro-rata (12mo)                               USD 2,400
   Stage 3 total                                            USD 8,470
```

   18.5.  Stage 4 — Standard Pathway

```
   Item                          Detail                    Cost
   ----------------------------  ------------------------  ------
   Specification writing          ~100hrs assisted          USD 200
   Conformance suite              ~60hrs Claude Code        USD 120
   Self-redesign pilot            Evaluate empirically      USD 100
   Community and outreach                                   USD 500
   Infrastructure (6mo)                                     USD 150
   Mac Studio pro-rata (6mo)                                USD 1,200
   Stage 4 total                                            USD 2,270
```

   18.6.  Total Cost Summary

```
   Stage                     Duration   Operating    Hardware    Total
   ------------------------  ---------  -----------  ----------  -------
   Stage 1 — Falsification   8 months   USD 200      USD 1,200   USD 1,400
   Stage 2 — Language Viab.  6 months   USD 1,900    USD 1,600   USD 3,500
   Stage 3 — Ecosystem Proof 12 months  USD 5,670    USD 2,400   USD 8,070
   Stage 4 — Standard Path.  6 months   USD 1,070    USD 1,200   USD 2,270
   Mac Studio (upfront)      —          —            USD 7,199   USD 7,199
   ------------------------  ---------  -----------  ----------  -------
   Total                     32 months                           USD 22,440
```

   The dominant operating cost is human review and DPO labelling
   (approximately USD 5,500 total).  The Mac Studio eliminates
   essentially all cloud training costs for 7B and 32B models.
   Total cloud GPU expenditure across the project is under USD 400.

---

## 19.  Self-Improvement Loop

   Once a working toke model exists, the generation and validation
   infrastructure becomes a continuous improvement engine operating
   without external intervention.

   **Automatic corpus expansion**

      The model generates tk programs continuously.  Programs passing
      compilation and testing are added to the corpus.  Programs
      failing are added as correction examples.  The corpus grows
      without human intervention for Phase A through C complexity.

   **Error pattern analysis**

      The compiler logs all structured errors with frequencies.  The
      top error patterns become targeted training examples.  If 12%
      of errors are "missing error propagation on database call",
      1,000 new examples demonstrating correct propagation are
      generated and added.  This analysis runs weekly, automatically.

   **Language redesign proposals**

      Once the model reliably generates corpus Phase C programs, it is
      prompted with its own error record:

```
      You have generated 100,000 tk programs.  These constructs
      appear most frequently in correction traces:
      [top 10 error-prone patterns].

      These constructs have >50% first-attempt error rate: [list].

      Propose modifications to the tk construct set that would:
      1.  Eliminate the top 3 error patterns.
      2.  Reduce average file token count by 15%.
      3.  Maintain backward compatibility where possible.

      For each proposal: before/after example, expected impact.
```

      Proposals are evaluated empirically: implement in the compiler,
      translate a representative corpus subset, measure error rate
      change.  Adopted changes are incorporated into the next language
      revision.  The model fine-tunes on the updated corpus.

   This constitutes a language that partially designs itself —
   grounded in empirical measurement rather than design intuition.

---

## 20.  Benchmark Targets

   The following table presents targets, evaluation context, and
   measured values.  Gate 1 evaluation completed 2026-04-03: PASS.

```
   Metric                    toke          Python       C            Notes
   -----------------------   -----------   ----------   ----------   ------
   Token efficiency          12.5% better  Baseline     Baseline     Gate 1 PASS
   First-pass compile (LLM)  92.3%         High         Medium       1000 tasks
   Pass@1 (held-out)         63.7%         N/A          N/A          Gate 1 PASS
   Repair iterations         Measuring     Medium       Medium       Phase 2
   End-to-end gen. cost      Measuring     Medium       Medium       Phase 2
   Binary performance        Native        Interpreted  Native       LLVM
   Startup time              <1ms          ~100ms       <1ms         No VM
   Memory (req. handler)     Arena/no GC   GC           Manual       Arena
   Training data available   ~47K corpus   Vast         Vast         Built
   Conformance tests         90/90         N/A          N/A          100%
   Compiler size             ~3,700 LOC    N/A          N/A          C99
```

   Gate 1 results: Token reduction 12.5% (8K vocab) / 13.1% (32K
   vocab) vs cl100k_base.  Pass@1 63.7% (588/923 compilable tasks)
   on 1,000 held-out benchmark tasks using Qwen 2.5 Coder 7B with
   QLoRA adapter.  Both criteria exceeded the required thresholds
   (>10% token reduction AND >=60% Pass@1).

   Full Gate 1 decision document: docs/gate1-decision.md

---

## 21.  Repository Structure

```
   tokelang/
   |-- tkc/
   |   |-- lexer.c            Lexer (~300 lines)
   |   |-- parser.c           Parser (~400 lines), LL(1)
   |   |-- type_checker.c     Type checking and arena validation
   |   |-- ir_lower.c         AST to toke IR (SSA form)
   |   |-- llvm_backend.c     toke IR to LLVM IR
   |   `-- test/              Conformance suite
   |
   |-- stdlib/
   |   |-- std.http.tk        HTTP types and route registration
   |   |-- std.db.tk          Database access
   |   |-- std.json.tk        JSON encode/decode
   |   |-- std.file.tk        File I/O
   |   `-- std.net.tk         Socket operations
   |
   |-- corpus/
   |   |-- generator/         Programmatic corpus generation
   |   |-- pipeline/          LLM generation + validation harness
   |   |-- judge/             Qwen local judge agent
   |   `-- diff_test/         Parallel 4-language differential testing
   |
   |-- tokenizer/
   |   |-- train.py           Purpose-built BPE tokenizer training
   |   `-- eval.py            Tokenizer evaluation protocol
   |
   |-- models/
   |   |-- finetune/          QLoRA training scripts (MLX)
   |   `-- eval/              Benchmark evaluation harness
   |
   |-- benchmark/
   |   |-- tasks/             Benchmark task definitions
   |   |-- hidden_tests/      Held-out test cases not in corpus
   |   `-- baselines/         Reference implementations
   |
   `-- spec/
       |-- grammar.ebnf       Formal grammar (normative)
       |-- semantics.md       Type rules and memory model
       `-- errors.md          Error code registry
```

   Each subdirectory MUST have a GitHub Actions CI pipeline.  The
   conformance suite MUST run on every commit to the tkc directory.
   Corpus validity checks MUST run nightly.

---

## 22.  Milestones

```
   ID    Deliverable                                          Target  Status
   ----  ---------------------------------------------------  ------  -------
   M0    Spec locked: 80-char set, symbols, keywords, EBNF       1    DONE
   M0.5  Mac Studio purchased and configured                      1    DONE
   M0.5  Qwen 2.5 Coder 32B running locally, pipeline tested     1    DONE
   M1    tkc lexer + parser, zero dependencies, LL(1)             2    DONE
   M2    Type checker + structured error output                   3    DONE
   M3    LLVM IR backend, hello world to native binary            4    DONE
   M4    stdlib core: http, db, json, file                        6    DONE
   M5    Corpus: ~47K programs across Phases A-D                  8    DONE
   M5.5  GATE 1: token efficiency + Pass@1 results                8    DONE (PASS)
   M6    7B fine-tune, first accuracy benchmark                  10    DONE
   M7    Phase B-C corpus expansion                              14    —
   M8    Production profile tokenizer trained                     16    —
   M9    Development models: 7B + 32B on full corpus             18    —
   M9.5  GATE 2: language viability results                       18    —
   M10   Phase D corpus + agent review pipeline                   22    —
   M11   Self-improvement loop running autonomously               24    —
   M12   Stage 3 models + full ecosystem                          26    —
   M12.5 GATE 3: multi-model, multi-task benchmark                26    —
   M13   Formal specification document                            28    —
   M14   Self-redesign pilot from error pattern analysis          30    —
   M15   Conformance suite + consortium proposal                  32    —
```

   M1 through M6 were completed ahead of schedule.  The 7B fine-tune
   (M6) was completed using QLoRA via Apple MLX on the development
   profile corpus (73K training examples, eval loss 0.158).  Gate 1
   evaluation completed 2026-04-03: PASS.  Token reduction 12.5%
   (threshold >10%), Pass@1 63.7% (threshold >=60%) on 1,000 held-out
   tasks.  The project proceeds to Phase 2 (Language Extensions).

---

## 23.  Open Questions

   The following design decisions are intentionally deferred to avoid
   premature commitment before empirical data is available.

   **Concurrency model**

      Async/await, goroutine-style lightweight threads, message
      passing, or structured concurrency?  The choice affects the
      type system and compiler significantly.  Deferred until the
      synchronous model is stable and Gate 2 validated.

   **Generics depth**

      Full parametric polymorphism requires significant type checker
      complexity and may reduce LLM generation reliability for
      unfamiliar type variable interactions.  Currently limited to
      built-in collection types.  Deferred until Stage 2 validation.

   **FFI surface**

      Minimal extern declarations suffice for Stage 1.  Full C ABI
      interop rules are designed in Stage 2 alongside the memory model
      extension needed to handle non-arena pointers.

   **Reserved characters ^ and ~**

      Not yet assigned.  Candidates: bitwise XOR and bitwise NOT.
      Will be assigned when a concrete language need arises, without
      requiring a new profile version.

   **Language self-redesign**

      The model will propose language construct revisions in Stage 4
      based on error pattern analysis of 100,000+ generated programs.
      The specification is deliberately kept open to that outcome
      rather than anticipated by human designers now.

---

## 24.  Security Considerations

   This section addresses security properties relevant to the use of
   toke in automated code generation contexts.

   **Sandboxing by language design**

      A toke program without FFI declarations has no direct system
      call access outside stdlib-mediated I/O, no access to arbitrary
      memory addresses, no ability to load dynamic libraries at
      runtime, and no metaprogramming or code execution facilities.
      These properties arise from the language design, not from an
      external sandbox.  LLM-generated toke programs that pass
      compilation are therefore safer to execute without human review
      than programs in languages with unrestricted system access.

   **Repair loop safety**

      The fix field in diagnostic records MUST only be populated when
      the suggested fix is deterministically correct.  An incorrect
      suggested fix applied by an automated repair loop could corrupt
      program semantics silently.  An incorrect fix field is a
      conformance defect.

   **Corpus contamination**

      Training data poisoning is a known attack vector for LLM
      fine-tuning.  The corpus generation pipeline MUST enforce
      strict holdout separation between training and test tasks.
      Deduplication MUST be applied before any entry is added.
      Hidden test cases MUST NOT appear in the training corpus.

   **Supply chain**

      The compiler and standard library are the root of trust for all
      generated programs.  The reference implementation MUST be open
      source with reproducible builds.  All build artefacts SHOULD
      carry cryptographic signatures.

   **FFI boundaries**

      Foreign function interface code (deferred feature) introduces
      the possibility of unsafe memory access.  When FFI is
      standardised, it MUST require explicit unsafe annotations at
      declaration sites, analogous to Rust's unsafe keyword.

---

## 25.  IANA Considerations

   This document does not require any IANA actions.

   If the .tk file extension or the tokelang package namespace require
   formal registration in future contexts, those registrations will
   be pursued through the relevant bodies at that time.

---

## 26.  Risks

```
   Risk                               Prob.   Mitigation
   --------------------------------   ------  ---------------------------
   Token savings marginal (<10%)      Medium  Gate 1 halts; pivot to
                                              typed-IR approach
   LLMs cannot learn new language     Low     Prior: specialisation on
                                              Zig, COBOL, domain-specific
                                              languages is well-documented
   Mac Studio insufficient for 32B    Low     Cloud A100 burst available;
   training                                   128GB has adequate headroom
   Qwen quality insufficient for      Medium  Verified in pipeline test
   pre-filtering                              at M0.5; fall back to
                                              API-only if needed
   Corpus contamination / test leak   Medium  Strict holdout,
                                              deduplication, hidden test
                                              rotation per WS4
   Tcl/Tk naming confusion            Low     Different case, different
                                              domain; tokelang package
                                              name resolves ambiguity
   Phase E review under-resourced     Medium  Reduce Phase E scope;
                                              increase automated
                                              structural checks
```

---

## 27.  References

### 27.1.  Normative References

   [RFC2119]  Bradner, S., "Key words for use in RFCs to Indicate
              Requirement Levels", BCP 14, RFC 2119,
              DOI 10.17487/RFC2119, March 1997,
              <https://www.rfc-editor.org/info/rfc2119>.

   [RFC8174]  Leiba, B., "Ambiguity of Uppercase vs Lowercase in
              RFC 2119 Key Words", BCP 14, RFC 8174,
              DOI 10.17487/RFC8174, May 2017,
              <https://www.rfc-editor.org/info/rfc8174>.

### 27.2.  Informative References

   [LLVM]     Lattner, C. and Adve, V., "LLVM: A Compilation
              Framework for Lifelong Program Analysis and
              Transformation", Proceedings of CGO 2004, March 2004.

   [WASM]     Haas, A., et al., "Bringing the Web up to Speed with
              WebAssembly", Proceedings of PLDI 2017, June 2017.

   [LLMLINGUA] Jiang, H., et al., "LLMLingua: Compressing Prompts
              for Accelerated Inference of Large Language Models",
              Proceedings of EMNLP 2023, December 2023.

   [SYNCODE]  Ugare, S., et al., "SynCode: LLM Generation with
              Grammar Augmentation", arXiv:2403.01632, March 2024.

   [MLIR]     Lattner, C., et al., "MLIR: A Compiler Infrastructure
              for the End of Moore's Law", arXiv:2002.11054, 2020.

   [QWEN]     Qwen Team, "Qwen2.5-Coder Technical Report",
              Alibaba Cloud, November 2024.

   [MLX]      Apple Machine Learning Research, "MLX: An Array
              Framework for Apple Silicon", GitHub, 2023.

---

## Appendix A.  Acknowledgements

   The design of toke draws on decades of work in programming language
   theory, compiler design, and LLM code generation research.
   Particular intellectual debts are owed to Arthur Whitney for the
   philosophy of density in language design, to the LLVM project for
   making native compilation accessible, and to the researchers behind
   LLMLingua, SynCode, and the Outlines project for establishing
   the field of LLM-constrained generation.

---

## Author's Address

   M. Karwalski
   tokelang.dev

   Email: spec@tokelang.dev
   URI:   https://github.com/tokelang/spec
