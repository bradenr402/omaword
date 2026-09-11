# Backend tests

From the repository root, build and run offscreen in `build/tests`:

```sh
bin/test
# Optional integration cases using your own downloaded edition:
OMABIBLE_TEST_EPUB="$HOME/Downloads/NETBIBLE21.epub" bin/test
```

`OMABIBLE_TEST_EPUB` is optional. Without it, real-edition integration cases are
skipped. Tests isolate application data in a temporary directory and read the
user-supplied EPUB in place. No Bible content is included in the test sources.

## Backend behavior

- Data lives under Qt's `QStandardPaths::AppDataLocation`: `bible.sqlite` is the
  replaceable, validated edition; `personal.sqlite` holds annotations and reading
  state with synchronous SQLite commits. Existing valid data opens without import.
- References support full names, standard aliases, ordinal/roman numbered books,
  compact forms, chapter-only references, same-chapter ranges, and single-chapter
  book shorthand. A range may span omitted verse numbers, but its endpoints must
  exist. Cross-chapter ranges are rejected with an explanatory error.
- Flags cover verses: a selection is flagged only when **all** its actual verses
  are covered. Toggling a partially covered selection flags all of it. Toggling
  a fully covered selection removes that flag within the selection, splitting
  existing ranges as needed, while preserving the other flag and all notes.
- Notes belong to an **exact range**. Selecting an overlapping range does not
  expose or overwrite that note. `hasNote` on each verse includes overlapping
  notes. Empty notes remove their entry unless a flag remains.
- No-selection `selectionReference` is the chapter reference; `copySelection()`
  copies that chapter. Annotation commands require an explicit verse selection.
- Backups contain personal data only, with format `omabible-backup`, version `1`,
  edition, UTC creation time, reading state, and annotation records. Restore
  replaces personal data in one transaction after strict field/type/range
  validation. Restore requires the imported edition to validate verse endpoints.
- Markdown export writes `omabible-notes.md` atomically in the chosen existing
  directory, containing references, flags, and unchanged Markdown notes.
- Import derives references from embedded verse markers, including cross-file
  chapter endings. It validates 66 book totals, all 1,189 chapters, 31,087 verses,
  and the NET's 16 intentionally omitted verse numbers (including the empty
  publisher marker for Acts 24:7). Translator superscripts
  are removed; editorial headings/psalm superscriptions/acrostic and speaker
  labels are kept in `heading`, separate from verse text. Poetry and intra-verse
  breaks are retained; HTML is generated from escaped text and safe inline tags.

## Import limits and chapter metadata provenance

XML is preflighted token-by-token, including discarded subtrees: maximum depth
64, formatting (`b`/`i`) depth 16, 100,000 elements per document, and entity
expansion limit 1,024. Expanded XML text and generated output each have cumulative
budgets of 2 Mi UTF-16 code units/document and 64 Mi/import. Generated verse
output has a 64 Ki budget (headings included); escaping is conservatively charged
before allocating output. Oversized headings are rejected before concatenation.
All failures precede installation of the staged database.

The numeric-only `chapterMaximum()` table was derived from the user's trusted
`NETBIBLE21.epub` by collecting embedded `chapter:verse` markers across archive
entries, rather than assigning markers according to XHTML titles. Every chapter
must have its exact maximum and exact count (maximum minus known omissions).
Metadata was independently checked on 2026-09-10 against the explicitly numbered
verses in [scrollmapper's KJV dataset](https://github.com/scrollmapper/bible_databases/blob/master/formats/json/KJV.json).
All 1,189 maxima match except these three NET versification differences, each
independently confirmed against BibleGateway's NET chapter markers:

- [2 Corinthians 13](https://www.biblegateway.com/passage/?search=2+Corinthians+13&version=NET): NET 13, KJV 14.
- [3 John 1](https://www.biblegateway.com/passage/?search=3+John+1&version=NET): NET 15, KJV 14.
- [Revelation 12](https://www.biblegateway.com/passage/?search=Revelation+12&version=NET): NET 18, KJV 17.

The older labs.bible.org API uses different numbering in the latter two chapters
and was therefore not used as the NET 2.1 authority. No downloaded Scripture text
is embedded in the sources. Tests additionally check each book's independent
total, all chapter counts, and a compensating Genesis 1:31 → Genesis 2:26
corruption that preserves the book total.

Navigation reads and validates the entire destination chapter before saving or
publishing state. Restore stages that chapter plus an annotation read-back within
its transaction, then publishes the already-staged models after commit without
additional SQL reads. Fault tests exercise chapter-body and annotation read-back
errors, verifying no signals/state changes and persistence after reopening.
Markdown and JSON exports share destination protection, including symlinks to
live databases and existing or dangling SQLite sidecars.
