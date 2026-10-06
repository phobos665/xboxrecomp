# Per-title section classes

One file per title, named for the title ID in its XBE certificate --
`<TITLEID>.json`, as in `config/seeds/`. `scripts/recompile.py` loads the
file matching the XBE it is given and passes it to `tools.disasm` as
`--data-sections`.

An XBE marks nearly every section executable, `.data` included. The
disassembler already treats the conventional PE data names as data; anything
else it sweeps as code. A title that links its assets into the image -- BLiNX
carries 42 demand-loaded model and map sections, 40 MB -- then gets thousands
of phantom functions decoded out of vertex data, each lifted, compiled and
linked.

    {"data": [{"pattern": "MDL*", "note": "why this is data"}]}

`pattern` is an fnmatch glob over section names. A matching section is loaded
as non-executable, so no detection pass treats an address in it as code.
Before adding one, check that nothing calls or jumps into it: the xrefs in
`<work-dir>/disasm/xrefs.json` with a `call` or `jump` type and a `to` inside
the section. The `note` should say what was checked.
