# USFM architecture

USFM is OmaWord's Scripture input. The parser accepts UTF-8 ZIP collections and
standalone `.usfm` / `.sfm` files, preserving source book/chapter/verse identity.

## Parser and identity

`src/usfmimporter.*` defines namespace `UsfmImporter`:

- `Edition`: `id`, `abbreviation`, `name`, `copyright` strings.
- `Result`: `ok`, `error`, `verseCount`, and `edition`.
- `inspectFile(path,error)`: reads resources and edition metadata without creating
  a database; full verse parsing occurs during import.
- `importFile(path,target,progress)`: parses into a temporary SQLite database,
  validates it, and publishes through `QSaveFile`. Progress uses shared atomic state.
- `validateDatabase(path,error)`: checks a text database read-only.

Numbered internal book filenames provide source ordering and edition suffix IDs.
`copr.htm` supplies title/rights and can supply or cross-check the source ID.
Consistent `\id` descriptions provide a title when needed. A title-derived
`usfm-<hash>` ID is used when no internal source ID is available. Outer ZIP names
do not establish identity. [VERSIONS.md](VERSIONS.md) lists catalog identities and
all seven copyright notices.

`BibleBooks::canon()` returns `Book {code,name,aliases}` entries;
`BibleBooks::bookId(name)` resolves a stable ID equal to index plus one. IDs 1–66
cover GEN–REV. Additional stable IDs are:

| Code | ID | Code | ID |
| --- | --- | --- | --- |
| TOB | 67 | SUS | 74 |
| JDT | 68 | BEL | 75 |
| ESG | 69 | 1MA | 76 |
| WIS | 70 | 2MA | 77 |
| SIR | 71 | 1ES | 78 |
| BAR | 72 | MAN | 79 |
| S3Y | 73 | 2ES | 80 |

IDs do not prescribe navigation order or chapter counts. Numbered source filenames
sort numerically, with path ordering as fallback. Imported `toc1`, `toc2`, `toc3`,
header names, canonical aliases, and USFM codes support edition-aware lookup.
This preserves OJB names and ordering. KJV+ has 80 reader books; `FRT`/`INT`
are ignored, and `ESG` chapters remain 10–16.

## Markers and validation

Chapter and verse markers must be strictly increasing integers 1–999. Partial
collections, gaps, and source omissions are accepted. Bridges and suffix labels
are rejected explicitly. Each retained verse has nonempty text; empty source
markers remain recorded without manufacturing reader verses. Every source chapter
must contain retained text.

Paragraphs, poetry, headings, supported inline emphasis, and within-verse breaks
are preserved. `\w` keeps the displayed word and following text while discarding
attributes such as Strong identifiers. Footnotes, cross-references, alternate
number labels, and introductions are excluded from Scripture text. Marker nesting
and closing styles are checked, including inside skipped notes. Unknown body
markers and malformed UTF-8 fail with a meaningful error.

Archive checks reject unsafe or duplicate paths, links, encryption, damaged entries,
and excessive expansion. Limits include 2,048 entries, 32 MiB per entry, 256 MiB
total/input, nesting depth 32, and 65,536 plain-text code units per verse/heading.
Failed parsing or validation preserves the destination.

## Text database and reader integration

Text schema **3** contains:

- `metadata`: source format/ID, edition, abbreviation, copyright, counts,
  paragraph-layout flag, source fingerprint, source-key digest, and content digest.
- `books(id,code,name,abbreviation,ordinal)` and `book_aliases(book,name)`.
- `source_chapters(book,chapter)` and
  `source_keys(book,chapter,number,retained)`.
- `verses(book,chapter,number,text,html,heading,poetry,paragraphStart)`.

Validation checks SQLite integrity, stable book identities, contiguous source
ordinals, chapter/key/retained-text consistency, metadata, counts, and digests.
Digests detect changes to the imported snapshot; they are not publisher signatures.

`BibleBackend::importUsfm` runs import asynchronously into a fresh text generation
and registers it transactionally. Dynamic books and chapter lists drive browsing,
range ordering, copying, and annotations. Personal-store schema 5 and backup
version 3 are separate from text schema 3. Shared-window drafts and library
publication are described in [IMPLEMENTATION.md](IMPLEMENTATION.md).
