# testing

A graded corpus for the decompiler, in two halves. Each half runs from level 01,
which anything should manage, to level 20, which is there to fail.

- `stress/` — programs that put pressure on the decompiler itself: control
  flow, types, calling conventions, aggregates, obfuscation.
- `crackmes/` — programs that hide an answer, so recovery can be judged by
  whether the recovered code still says yes to the right key. `ANSWERS.md`
  holds the keys, because this is a regression suite and not a puzzle site.

Every program checks itself. A stress program returns 0 when its own assertion
holds; a crackme prints `correct` for its key and `wrong` for anything else. If
one of them fails on its own, the corpus is broken, not the decompiler.

## Running it

```sh
./build.sh              # compile everything, with symbols and stripped
./build.sh -O2          # or at another optimisation level
./run.sh                # decompile each one and report
./run.sh --stripped     # against the copies with no symbols
./run.sh --only crackmes
```

`run.sh` asks three questions of each program.

| Column | Question |
|---|---|
| decompiles | did Astral produce output at all |
| compiles | does that output build with `cc -std=c11` |
| behaves | for a crackme, does the recovered check still accept its own key |

The third is the one that matters. Output that compiles but answers differently
is not a decompilation, it is a plausible-looking guess. To ask it, the unit's
own `main` is renamed away and the recovered `check` is called directly from a
separate driver.

## What it currently says

Astral decompiles all forty and compiles thirty-nine. Ten of the twenty
crackmes come back with logic that still works.

The failures are worth reading, because each names a real limit:

- **Reading absolute addresses.** A recovered function that indexes a table or
  a string constant refers to an address in the original image. Nothing maps
  that address in the rebuilt program, so it faults. Emitting the data a
  function reads would fix most of the ten.
- **`main` has no arguments.** Astral does not recover `argc` and `argv`, so a
  rebuilt program cannot be handed its own arguments. That is why the harness
  drives `check` directly rather than running the program.
- **Types that disagree with themselves.** Level 19 recovers `check` taking a
  `char *` but passes an integer at one call site, which modern compilers
  reject outright.

## Adding to it

Keep a level self-checking and self-contained: standard C, no dependencies
beyond libc, no network, nothing that reaches outside the process. A crackme
should state its answer in `ANSWERS.md` so the harness can verify it, and
should be about arithmetic and structure rather than about defeating analysis
tools.
