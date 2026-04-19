# Cookbook: CLI Tool with Subprocesses

## Overview

Build a command-line tool that reads configuration from environment variables, spawns subprocesses, and logs everything with structured output. Combines `std.env`, `std.log`, and `std.process` to show the standard pattern for writing CLI utilities in toke.

## Modules Used

- `std.env` -- Environment variable access and CLI argument parsing
- `std.log` -- Structured logging with levels and fields
- `std.process` -- Subprocess spawning and output capture

## Complete Program

```toke
I std.env;
I std.log;
I std.process;
I std.str;
I std.file;

(* Read configuration from environment variables *)
let work_dir = env.get("WORK_DIR") catch |_| { "/tmp/build"; };
let log_level_str = env.get("LOG_LEVEL") catch |_| { "info"; };
let max_retries = str.to_int(env.get("MAX_RETRIES") catch |_| { "3"; })!;

(* Map the string log level to the enum *)
let level = if log_level_str == "debug" {
    log.Debug;
} else if log_level_str == "warn" {
    log.Warn;
} else {
    log.Info;
};

(* Configure the logger *)
let logger = log.new(log.Config{
    level: level;
    format: log.Text;
    output: log.Stderr;
});

(* Parse CLI arguments *)
let args = env.args();
if env.arg_count(args) < 2 {
    log.error(logger; "usage: build-tool <command>"; log.Fields{});
    process.exit(1);
};
let command = env.arg(args; 1);

log.info(logger; "starting build tool"; log.Fields{
    "command": command;
    "work_dir": work_dir;
    "max_retries": str.from_int(max_retries);
});

(* Ensure the working directory exists *)
file.mkdir_all(work_dir)!;

(* Run a subprocess with retry logic *)
fn run_with_retry(cmd: Str; retry_args: @(Str)): process.Result!process.Err {
    let attempt = 0;
    loop {
        attempt = attempt + 1;
        log.info(logger; "running command"; log.Fields{
            "cmd": cmd;
            "attempt": str.from_int(attempt);
        });

        let result = process.run(process.Command{
            program: cmd;
            args: retry_args;
            cwd: work_dir;
            env: process.inherit_env();
        });

        match result {
            ok(r) => {
                if process.exit_code(r) == 0 {
                    log.info(logger; "command succeeded"; log.Fields{
                        "cmd": cmd;
                        "stdout_len": str.from_int(str.len(process.stdout(r)));
                    });
                    ret ok(r);
                };
                log.warn(logger; "command failed"; log.Fields{
                    "cmd": cmd;
                    "exit_code": str.from_int(process.exit_code(r));
                    "stderr": process.stderr(r);
                });
            };
            err(e) => {
                log.error(logger; "command error"; log.Fields{
                    "cmd": cmd;
                    "error": process.err_msg(e);
                });
            };
        };

        if attempt >= max_retries {
            log.error(logger; "max retries exceeded"; log.Fields{"cmd": cmd});
            ret err(process.Err{msg: "max retries exceeded"});
        };
    };
};

(* Execute the requested command *)
let result = run_with_retry(command; @("--verbose"))!;
let output = process.stdout(result);
file.write_text(str.concat(work_dir; "/last_output.log"); output)!;

log.info(logger; "build tool complete"; log.Fields{});
process.exit(0);
```

## Step-by-Step Explanation

1. **Environment config** -- `env.get` reads environment variables with fallback defaults via `catch`. This lets the tool run with sensible defaults while allowing override.

2. **Logger setup** -- The log level is configurable via `LOG_LEVEL`. The logger writes structured text to stderr so stdout remains clean for piping.

3. **Argument parsing** -- `env.args` returns the CLI arguments. The tool expects at least one argument specifying the command to run.

4. **Retry loop** -- `run_with_retry` executes a subprocess up to `max_retries` times. Each attempt is logged with its number. Non-zero exit codes trigger a retry.

5. **Subprocess execution** -- `process.run` spawns the command with arguments, a working directory, and inherited environment variables. The result includes stdout, stderr, and exit code.

6. **Output capture** -- On success, stdout is written to a log file in the working directory. The tool exits with code 0 on success or propagates the error.
