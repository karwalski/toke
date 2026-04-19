# Cookbook: LLM Content Pipeline

## Overview

Load a prompt template from disk, fill in variables, send it to a large language model, and write the generated output to a file. Combines `std.llm`, `std.file`, and `std.template` to build a reusable content generation pipeline.

## Modules Used

- `std.llm` -- Large language model API calls
- `std.file` -- File system read and write
- `std.template` -- String template rendering with variables

## Complete Program

```toke
I std.llm;
I std.file;
I std.template;
I std.str;

(* Read the prompt template from disk *)
let tmpl_src = file.read_text("prompts/summarise.tmpl")!;

(* Parse the template *)
let tmpl = template.compile(tmpl_src)!;

(* Define the variables to inject *)
let vars = template.new_vars();
template.set(vars; "topic"; "the history of railway signalling");
template.set(vars; "tone"; "conversational");
template.set(vars; "max_words"; "300");

(* Render the final prompt string *)
let prompt = template.render(tmpl; vars)!;

(* Configure the LLM client *)
let client = llm.new_client(
    model: "claude-sonnet";
    max_tokens: 1024;
    temperature: 0.7;
);

(* Send the prompt and get a response *)
let response = llm.complete(client; prompt)!;
let text = llm.content(response);

(* Write the result to an output file *)
let out_path = str.concat("output/summary_"; str.concat(
    str.from_int(file.timestamp_now());
    ".md";
));
file.write_text(out_path; text)!;

(* Also write a metadata sidecar with token usage *)
let usage = llm.usage(response);
let meta = str.concat("input_tokens: "; str.concat(
    str.from_int(llm.input_tokens(usage));
    str.concat("\noutput_tokens: "; str.from_int(llm.output_tokens(usage)));
));
file.write_text(str.concat(out_path; ".meta"); meta)!;
```

## Example Template File

The file `prompts/summarise.tmpl` might contain:

```
Write a {{tone}} summary about {{topic}}.
Keep it under {{max_words}} words.
Use clear headings and short paragraphs.
```

## Step-by-Step Explanation

1. **Load template** -- `file.read_text` reads the entire template file into a string. The `!` unwraps the result or propagates any IO error.

2. **Compile template** -- `template.compile` parses the `{{var}}` placeholders into an efficient internal representation that can be rendered multiple times.

3. **Set variables** -- `template.new_vars` creates a variable map. `template.set` binds each placeholder name to a concrete string value.

4. **Render prompt** -- `template.render` substitutes all variables and returns the final prompt string ready for the LLM.

5. **Call LLM** -- `llm.new_client` configures model, token limit, and temperature. `llm.complete` sends the prompt and blocks until the response arrives.

6. **Write output** -- The generated text is written to a timestamped file. A metadata sidecar records token usage for cost tracking.
