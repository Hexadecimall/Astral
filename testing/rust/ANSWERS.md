# Keys

Kept here rather than in the sources, so a run against the corpus is a real
test of the check rather than a search for the answer beside it.

| Program | Key | What it exercises |
| --- | --- | --- |
| 01_plain | `astral` | `==` on `&str`: a length test, then the bytes |
| 02_bytes | `nebula` | a slice, so the loop bound is carried rather than written |
| 03_xor | `quasar` | the key is stored xored, never as itself |
| 04_checksum | `starfish` | only the number the key adds up to survives |
| 05_iterator | `orbital` | `enumerate().map().fold()` with a closure, all inlined |
| 06_state | `ze9o` | an enum and a `match`, which is a jump table |

Each takes the key as its only argument, prints `correct` or `wrong`, and exits
0 or 1. The same shape as the C corpus, so the two can be measured the same way.
