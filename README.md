# cruftar

`cruftar` is the package manager and project CLI for the Cruft systems language. It is written in Cruft. The command is **`cruftar`**. `cruftc` is the compiler this tool invokes; it is not this program. (A)

```powershell
cruftar new demo --template pure-cruft --target cli
cd demo
cruftar check
cruftar build
cruftar run
cruftar test
```

## Installation

A Windows release contains `cruftar.exe`, `cruftc.exe`, the Cruft standard library, and the compiler runtime (`arena.c`, `mem.c`, `fs.c`, `io.c`), plus the LLVM libraries `cruftc` links. Linking a program uses `gcc` or `clang` from `PATH`.

### PowerShell

```powershell
irm https://raw.githubusercontent.com/yakupcemilk/cruftar/main/install.ps1 | iex
```

The script downloads the latest GitHub release, extracts it to `%USERPROFILE%\.cruft`, and appends `%USERPROFILE%\.cruft\bin` to the user `PATH`. Open a new terminal and run `cruftar help`.

### Manual download

Download `cruftar-v*-windows-x64.zip` from the [releases page](https://github.com/yakupcemilk/cruftar/releases). Extract it so this tree exists:

```text
%USERPROFILE%\.cruft\
  bin\cruftar.exe
  bin\cruftc.exe
  std\mem.cruft
  src\runtime\
```

Add `%USERPROFILE%\.cruft\bin` to the user `PATH`. `cruftar` finds `cruftc.exe` there. `std` and `src\runtime` sit next to `bin`, which is where this tool looks for them.

Pushing a `v*` tag builds that archive and publishes the release.

## Build cruftar

You need `cruftc`, a C compiler (`gcc` or `clang`), and the Cruft standard library. From this repository:

```powershell
$env:CRUFTC = "C:\path\to\cruft\Cruft\build\cruftc.exe"
.\build.ps1
.\bin\cruftar.exe help
```

`build.ps1` compiles `src/*.cruft` with `cruftc` and links `src/runtime/host.c` plus the compiler runtime (`mem.c`, `fs.c`, `io.c`). The host C file is only the operating-system bridge: Cruft's `std/fs` and `std/io` cover files and streams, and the bridge supplies the process command line, directories, and `cruftc` launches. Command behavior lives in the `.cruft` sources.

Put `bin` on `PATH` if you want to type `cruftar`.

## How cruftc is found

1. `--cruftc <path>`
2. The `CRUFTC` environment variable
3. `cruftc` on `PATH`
4. `build/cruftc.exe` under the project, a parent directory, or a sibling `Cruft` directory

The standard library is the directory that contains `mem.cruft`:

1. `CRUFT_STD`
2. `std/` next to the compiler sources

`CRUFT_RUNTIME` overrides `src/runtime` next to the compiler. Host executables are linked with `CC`, then `gcc`, `clang`, or `cc`.

`NO_COLOR` disables color and animated spinners. A non-terminal gets one plain status line per stage.

## Commands

| Command | Exit | What it does |
|---|---|---|
| `cruftar new <name>` | 0, 1, or 2 | Scaffold a project |
| `cruftar init` | 0 or 1 | Write `cruft.toml` here. Refuses to overwrite it unless `--force` |
| `cruftar build` | compiler status, or 1 | `--check`, LLVM IR, object file, and a host link for native binaries |
| `cruftar run` | program status | Build, then execute. Extra arguments go after `--` |
| `cruftar check` | compiler status | `cruftc --check` only |
| `cruftar test` | 1 if any test fails | Run every `tests/*.cruft` program |
| `cruftar add <pkg>` | 0 or 2 | Edit the manifest. No network |
| `cruftar clean` | 0 | Delete `build/` and `.ll` / `.o` files inside the project |
| `cruftar help` | 0 | Command list |
| `cruftar --version` | 0 | Print `0.1.0` |

Usage errors exit 2. A missing compiler is an error. Compiler failures keep `cruftc`'s exit code.

`cruftar build` uses `dev-opt-level` (`"0"`). `--release` uses `release-opt-level` (`"2"`).

## Wizard

On a terminal, `cruftar new <name>` asks two questions. The up and down arrows move, Enter confirms, and `1` through `4` select a row. `q` aborts.

Templates:

1. `pure-cruft` — no C or Rust dependency
2. `cruft-with-c` — `extern fn`, `src/runtime/` stubs, and `[link]` flags
3. `cruft-with-rust` — a Cargo staticlib with `#[no_mangle] extern "C"`

Targets:

1. `cli` — streams. Pure Cruft takes a command code. C and Rust templates read the process command line
2. `lib` — `pub` / `pri` and `export`. No process entry point
3. `gui` — a text frame, or a `cruft_gui_open` / `cruft_gui_close` scaffold. No window system is required
4. `wasm` — `[target] triple = "wasm32-unknown-unknown"`

Non-interactive:

```powershell
cruftar new demo --template cruft-with-c --target gui
```

If stdin is not a terminal and a flag is omitted, cruftar defaults that flag to `pure-cruft` or `cli` and says so.

Every combination writes `cruft.toml`, `README.md`, `.gitignore`, `src/`, `tests/`, and `examples/`. C templates also write `src/runtime/`. Rust templates also write `rust/`.

`new` does not run `cargo`. `cruftar build` of a native `cruft-with-rust` binary runs `cargo build --release` and fails with an explicit error if `cargo` is missing. Wasm and library builds record that the staticlib was skipped when `cargo` is absent. `cruftar test` needs `cargo` for Rust templates, because the host test executable links the staticlib.

## Manifest

`cruftc` parses comments, quoted strings, and arrays of quoted strings. Unknown keys inside the sections it knows are ignored. Any other section makes `cruftc` warn, so cruftar keeps its own fields under `[package]`.

```toml
[package]
name = "hello"
version = "0.1.0"
edition = "2026"
kind = "bin"                 # bin | lib
template = "pure-cruft"      # pure-cruft | cruft-with-c | cruft-with-rust
target = "cli"               # cli | lib | gui | wasm
entry = "src/main.cruft"
std = "std"
opt-level = "2"
dev-opt-level = "0"
release-opt-level = "2"

[target]
triple = ""                  # wasm uses wasm32-unknown-unknown

[link]
libraries = []               # passed as -l
search_paths = []            # passed as -L

[modules]
paths = ["src"]

[dependencies]
io = "std"                   # cruftc preloads the key
```

`cruftar add` understands:

- `std/io` → `[dependencies] io = "std"`
- `c:raylib` or `--lib raylib` → `[link] libraries`
- `rust:mycrate` → `[link] libraries`
- `path/libfoo.a` → a search path plus link name `foo`
- `--search dir` → `[link] search_paths`

Adding the same entry twice does not duplicate it.

## Tests

There is no `test` attribute in Cruft. Each file in `tests/` is its own program with `pub fn main(): i32`. Return `0` for pass. cruftar compiles that file together with every `src/**/*.cruft` file that does not define the process entry, then links and runs a host executable. Wasm projects are tested on the host triple.

## What the compiler runs

```text
cruftc --check --manifest cruft.toml --lib <std> -O0 src/....cruft
cruftc -emit-llvm -O0 -o build/<name>.ll --manifest cruft.toml --lib <std> src/....cruft
cruftc -emit-llvm -O<n> -o build/<name>.ll ...    # when the selected opt level is above 0
cruftc -c -O<n> -o build/<name>.o ...
gcc -o build/<name>.exe build/<name>.o <runtime .c> -L... -l...
```

`--check` is a real `cruftc` flag. `-O0` through `-O3` select the LLVM pipeline. The arena pass runs inside `cruftc` before codegen. Wasm builds pass `--target wasm32-unknown-unknown` and stop after the object file.

## License

MIT. See [LICENSE](LICENSE).
