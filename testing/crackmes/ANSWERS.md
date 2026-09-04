# Crackme answers

This is a regression corpus, not a puzzle site: the answers live here so
the harness can check that each program still accepts its own key, and that
a decompiled copy behaves the same. Read them only when you want to.

| Level | What makes it hard | Key |
|---|---|---|
| 01 | a literal compared with strcmp | `astral` |
| 02 | compared one character at a time | `orbit` |
| 03 | length and a sum of characters | `abcdef` |
| 04 | exclusive-or with one byte | `nebula` |
| 05 | exclusive-or with a key that advances | `cluster` |
| 06 | a substitution table applied per character | `quasar` |
| 07 | each character checked by its own arithmetic | `photon` |
| 08 | constraints that tie characters together | `mirror` |
| 09 | decoded from a packed form before comparison | `lantern` |
| 10 | a cyclic redundancy check over the whole key | `vector` |
| 11 | two independent checks that must both hold | `cascade7` |
| 12 | a state machine walked by the key | `abcabcab` |
| 13 | a bit pattern the key must produce | `binary` |
| 14 | a hash of the key compared with a constant | `gravity` |
| 15 | compared against a generated sequence | `trhgue` |
| 16 | a table built at run time, then indexed by the key | `indexed` |
| 17 | a system of equations over the characters | `system` |
| 18 | the check itself is a flattened state machine | `flatten` |
| 19 | a small virtual machine that validates the key | `machine` |
| 20 | a virtual machine over a hash over a generated table | `layercake` |
