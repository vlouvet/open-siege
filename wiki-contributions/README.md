# Wiki contributions

Updated copies of upstream wiki pages enriched with concrete byte-level findings from this branch's work. Held here because GitHub wikis have no PR mechanism and this fork's wiki is not enabled — these files are staged for manual submission to the upstream wiki at https://github.com/open-siege/open-siege/wiki when a contributor with write access wants to take them.

## Files

| File | Change vs upstream | Source of new info |
|---|---|---|
| [DML.md](DML.md) | Adds "Role in the engine" section clarifying DML is metadata, not load-bearing for static texturing; notes that `lib3space` does not currently parse DML | Cross-referencing wiki, `lib3space` API, and `darkstar.md`'s engine format list (DML not on it) |
| [PPL.md](PPL.md) | Replaces a 3-line redirect with the actual two PPL variants and the observed PL98 byte layout | Hex dump of `Shell.ppl` from Starsiege: Tribes 1.41 freeware |
| [BMP.md](BMP.md) | Adds explicit "Which variant a given game uses" section — Starsiege/Tribes is **always PBMP, never Windows BMP** — plus the observed PBMP `head`-chunk byte layout | Magic-byte inspection of every `.bmp` in `Entities.vol` from Starsiege: Tribes 1.41 freeware |
| [PAL.md](PAL.md) | Adds a "Disambiguating variants on disk" table (multiple formats share the `.pal`/`.ppl` extension) and the observed PL98 layout | Same source as PPL.md, plus the variants list already in the page |

## Provenance and method

The byte-level observations come from extracting individual files out of the Starsiege: Tribes 1.41 freeware release (the official 2004 Sierra/VUGames public release, `tribes_gsi.exe`) using the `examples/vol-list --extract` tool in this same branch, then `xxd` / `od` on the resulting files.

Everything documented here is reproducible from publicly available freeware. No proprietary or copyrighted asset content is included — only file-format header bytes, which are uncopyrightable factual technical information.

The new wording is original to this branch and licensed under the same MIT terms as the rest of Open Siege.

## Why these pages specifically

The wiki review during this branch's work found that the upstream wiki is largely skeletal — many pages are empty stubs or 1-10 line summaries. The four pages updated here are the ones where:

1. We had concrete new byte-level findings, AND
2. The content is on the critical path for texturing Tribes models (which is the next obvious capability beyond `dts-viewer`'s current flat-shaded rendering).

See [`examples/TEXTURING-NOTES.md`](../examples/TEXTURING-NOTES.md) for how these findings tie together into a workable texturing pipeline.

## How to submit upstream

Anyone with write access to https://github.com/open-siege/open-siege/wiki can:

```sh
git clone https://github.com/open-siege/open-siege.wiki.git
cp wiki-contributions/*.md open-siege.wiki/
cd open-siege.wiki
git diff   # review
git add DML.md PPL.md BMP.md PAL.md
git commit -m "Document PBMP/PL98 byte layouts for Tribes; clarify DML role"
git push
```

Alternatively, paste each updated page's contents into the wiki's web editor at https://github.com/open-siege/open-siege/wiki/<PageName>/_edit and save.
