# Contributed databases

Every `.astral` file here is compiled into Astral's knowledge base at build
time, so merging one is all it takes for the names in it to reach everyone.

Files arrive through `astral contribute database`. They are named after a hash
of their own contents, which identifies the data and nothing about whoever sent
it.

A file may only contain the record kinds `contrib/policy.astral` permits:
fingerprints, prototypes, and the rules that say what evidence means. Nothing
that names a path, an address or a person belongs here.
