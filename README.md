# candymountain-fuse

`ixmount`: a FUSE3 filesystem that mounts a
[candymountain](https://github.com/taxfromdk/candymountain) `ixserve`
frame server read-only, so any program that reads files can browse the
recordings and read frames in every format the server produces.

    make
    build/bin/ixmount http://localhost:8080 mnt
    fusermount3 -u mnt

Needs libfuse3 and libcurl. The link to candymountain is HTTP only; `make
test` needs a built candymountain checkout beside this folder (or
`CANDYMOUNTAIN=/path`). `CLAUDE.md` documents the tree and the design.
