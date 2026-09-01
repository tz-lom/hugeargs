# hugeargs

`hugeargs` is a lightweight Linux workaround for `E2BIG` / "Argument list too long" failures when tools spawn subprocesses with very large command lines or environments.

Typical example:

```text
g++: error trying to exec '.../cc1plus': execv: Argument list too long
```

The project provides:

- A preload library `libhugeargs.so` that intercepts process-spawn APIs.
- A helper wrapper script `./hugeargs` for simple command usage.
- Tests that reproduce and validate large-argument behavior.

## How It Works

1. Intercepted exec/spawn calls measure total exec payload size (`argv` + `envp`).
2. If payload is larger than the threshold (~1.9 MB), arguments/environment are serialized to a temporary file.
3. The target process is re-executed with a compact argument:
   - `--HUGEARGS_PLEASE_LOAD_ARGUMENTS_FROM_FILE=<path>`
4. At process startup, `__libc_start_main` hook restores arguments and merges file environment with current environment (file values win on key conflicts).

## What Is Intercepted

The library wraps these APIs:

- `execve`
- `__execve`
- `execveat`
- `fexecve`
- `execv`
- `execvp`
- `execvpe`
- `posix_spawn`
- `posix_spawnp`
- `__libc_start_main`

## Build

From project root:

```bash
make
```

This builds `libhugeargs.so`.

## Basic Usage

Use the wrapper script to run a command through hugeargs:

```bash
./hugeargs <executable> [args...]
```

Example:

```bash
./hugeargs g++ -o test/build/example <very-long-include-list> test/example.cpp
```

## Advanced Usage (Direct LD_PRELOAD)

You can also run binaries with the preload library directly:

```bash
LD_PRELOAD=./libhugeargs.so <command> [args...]
```

## Environment Variables

- `HUGEARGS_TMPDIR`
  - Directory used for temporary argument payload files.
  - Required for redirection path.
- `HUGEARGS_IGNORE`
  - Colon-separated executable basenames to exclude from redirection.
  - Example: `HUGEARGS_IGNORE=python:node`

## Testing

Run project tests:

```bash
make test
```

The test script validates:

- Large-argument failure reproduction without hugeargs.
- Recovery via preload/wrapper path.
- Environment merge and file-priority behavior for packed format.
- Practical `g++`/`cc1plus` case.

## Notes

- This project targets Linux/glibc-style dynamic linking behavior.
- Temporary files are created in `HUGEARGS_TMPDIR` and cleaned up by wrapper traps.
- The current `./hugeargs` script is intentionally simple; most policy logic resides in `libhugeargs.so`.
