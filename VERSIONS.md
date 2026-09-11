# Multi-version implementation contract

Annotations are version-specific; switching stays at the same canonical reference,
falling back to the chapter with feedback if an endpoint is absent. Retained
annotations are hidden until the same stable version ID is reimported. Existing
NET text and annotations must survive migration. Preferences remain global.

## Importer interface (src/epubimporter.*)

Existing NET metadata helpers and importFile(path,target,progress) remain available.
`struct Edition { QString id, abbreviation, name, copyright; };` describes an edition.
`Edition inspectFile(const QString &path, QString *error=nullptr);`
returns empty id on failure, without database writes. Stable IDs `net-2.1`,
`kjv-2006`. Result includes `Edition edition`. Database metadata stores `version_id`,
`abbreviation`, `edition`, `copyright`. Existing NET databases with edition only
remain valid. validateDatabase understands NET and KJV chapter counts separately.
NET parser preserved; KJV Haiola/eBible adapter preserves supplied words, strips
explicit note subtrees, handles Psalm headings and poetry, validates 31,102 verses.
Never accept damaged OCR study-Bible pages as Scripture. Both the original local
NKJV study EPUB and the exact replacement supplied for assessment have been
assessed as unsupported OCR layouts: unreliable reading order and absent supported
semantic verse markers prevent native Scripture import. A supported structured
edition is required; an `.epub`
extension alone does not establish compatibility. NIV remains unimported and
unsupported; any sample assessment is inspection-only.

## Backend interface (src/biblebackend.*)

`edition` uses NOTIFY versionChanged, not CONSTANT.
Properties `QString versionId`, `QString versionAbbreviation`, and
`QString versionCopyright` also use NOTIFY versionChanged. `QVariantList versions`
contains maps `{id, abbreviation, name, active}` with NOTIFY versionsChanged.
Invokables `bool switchVersion(QString id)` and
`bool removeVersion(QString id, bool removeAnnotations)`.
Existing importEpub identifies the edition, imports locally, and activates it
after success (same passage fallback), leaving all other editions intact.
All annotations, Saved, copy, navigation, backup/restore are active-version aware.
Use loaded edition's actual verse keys for canonical offsets; never NET-only
omission metadata when reading another version. Persist references, not offsets.
Reimport reconnects retained annotations. Remove active switches to another
installed version or ready=false if none. Removing last version must leave
preferences and retained notes intact. Migration and operations are atomic,
preserve old v1/v2 backups as NET, and protect all live databases from export.

### Storage and backups

Root `personal.sqlite` uses schema 3. `annotations` and `reading` are keyed by
`version_id`; `preferences` remains global. `installed` stores stable ID, path,
abbreviation, name, and copyright; `library` stores the active ID. Paths are
root-relative filenames, with new text generations at `bible-<UUID>.sqlite` in
the data root. The library is represented by tables, not a per-version directory
tree. Migration validates/registers legacy root `bible.sqlite` in place as NET,
assigns old annotations/reading to `net-2.1`, and commits transactionally.
Corrupt or unsafe legacy text remains unregistered while personal-data migration
completes; supported NET reimport reconnects the retained NET annotations.
New generations are validated before the registry transaction; obsolete text is
removed after commit. Startup restores the active version's saved reading state;
ordinary switching follows the current passage rather than a different version's
previous location. Removing annotations also removes that ID's reading state.
Deletion targets must be owned root text generations, or legacy `bible.sqlite`
for NET, with matching edition identity and an unambiguous registry entry.
Symlinks, hardlinks, non-regular files/sidecars, personal-store aliases, and
unrelated filenames are rejected; ownership is rechecked before cleanup.

New backups use format `omabible-backup`, version **3**, and include `versionId`
and `edition` alongside creation time, reading state, and annotations. They
contain only the active version's personal data. Restore requires that version
to be installed **and active**, with matching stable ID and edition name.
Legacy version-1/version-2 backups require active NET 2.1. Endpoint validation and
replacement of only the active version's annotations/reading commit together;
other versions and global preferences are preserved. Markdown export also scopes
to the active version and identifies its name/ID. No backup includes Bible text,
EPUBs, or preferences.

## UI (qml/Main.qml + tests/tst_ui.cpp)

Font default/reset 18, range 8..40, steps of 1 before desktop scaling; session-local.
Version button displays the active abbreviation immediately right of location;
combined controls centered and responsive. Shift V and Switch version command
open keyboard picker; Enter selects and closes, Shift Enter does nothing.
Remove version wizard: choose edition, remove annotations? (default keep), final
confirmation. Cancel/Esc cancels removal at every step. Both picker/wizard support empty
and busy states, safe notes flushing. Add version guide states supported EPUB
layouts precisely and explains OCR rejection and local-storage independence.
The reader edition label and About use active name/copyright; empty rights are
hidden, and the NET permissions link is visible only for NET. The command and
setup label is **Add version**.
The guide confirms both assessed NKJV OCR layouts are unsupported. Save annotation
backup explains active-version-only scope before opening the file dialog, including
separate backups per edition and reimport before backing up retained annotations.
Restore confirmation names the active edition and replacement scope. CLI `--import`
help explicitly names supported NET 2.1 and Haiola/eBible KJV 2006 EPUBs.

## Supported inputs and verification

- NET: official `NETBIBLE21.epub`, stable ID `net-2.1`, 31,087 verses.
- KJV: structured Haiola/eBible `eng-kjv2006.epub`, stable ID `kjv-2006`,
  31,102 verses. Both editions have 66 books and 1,189 chapters.
- NKJV original and replacement: rejection-only OCR fixtures, never successful
  imports. NIV is not an imported fixture.

These are layout/metadata contracts, independent of any developer's local paths.
See [tests/README.md](tests/README.md) for metadata, optional fixture environment
variables, and standalone commands for the importer-version test project.
`bin/test` automatically runs it after backend, UI, and block-annotation suites,
normalizes `OMABIBLE_TEST_KJV`, and exports it as `OMABIBLE_TEST_KJV_EPUB` when the
latter is unset/empty. NKJV rejection inputs still require explicit environment
variables.
No copyrighted Bible content belongs in the repository.
