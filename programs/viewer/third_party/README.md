# pl_mpeg dependency

Viewer 3.1 uses `pl_mpeg` by Dominic Szablewski for MPEG-PS demuxing,
MPEG-1 Video decoding and MPEG-1 Layer II (MP2) decoding.

Pinned commit:

`c871f2be022ece7ef4f64230b4fb8e1fb9eb6023`

Expected Git blob for `pl_mpeg.h`:

`b42f53f89f868a697435ab1e15742f370819ffbc`

Run:

```sh
python3 tools/fetch_pl_mpeg.py
```

or simply run `make`; the Makefile invokes the helper when
`third_party/pl_mpeg.h` is absent. The helper verifies the Git blob before
writing the file.

The upstream source identifies itself as MIT licensed. A copy of the MIT terms
used for redistribution is included in `PL_MPEG_LICENSE.txt`.
