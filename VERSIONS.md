# USFM versions and attribution

OmaWord's bundled catalog contains seven metadata entries. Each Install action
fetches one USFM ZIP directly over HTTPS. The URL pattern is
`https://ebible.org/Scriptures/<source-id>_usfm.zip`; details and permissions are at
`https://ebible.org/find/details.php?id=<id>&all=1`. There is no service API or account.
Browsing the catalog and reading installed editions work offline.

| Source ID | Catalog name | Catalog abbreviation | Direct USFM ZIP |
| --- | --- | --- | --- |
| `engnet` | NET Bible | NET | [Download](https://ebible.org/Scriptures/engnet_usfm.zip) |
| `eng-kjv2006` | King James (Authorized) Version | KJV | [Download](https://ebible.org/Scriptures/eng-kjv2006_usfm.zip) |
| `eng-kjv` | King James Version + Apocrypha | KJV+ | [Download](https://ebible.org/Scriptures/eng-kjv_usfm.zip) |
| `engojb` | The Orthodox Jewish Bible | OJB | [Download](https://ebible.org/Scriptures/engojb_usfm.zip) |
| `engylt` | Young's Literal Translation | YLT | [Download](https://ebible.org/Scriptures/engylt_usfm.zip) |
| `eng-asv` | American Standard Version (1901) | ASV | [Download](https://ebible.org/Scriptures/eng-asv_usfm.zip) |
| `engbsb` | Berean Standard Bible | BSB | [Download](https://ebible.org/Scriptures/engbsb_usfm.zip) |

## Copyright and source metadata

Catalog notices come from `resources/catalog.json`. During import, the archive's
`copr.htm` supplies its title and rights notice, including applicable license terms;
the installed reader and About use that imported metadata. All seven sources carry
attribution. HTML entities are decoded, and site-generation footer text is excluded.

| Source | Catalog attribution summary |
| --- | --- |
| NET | Copyright © 1996–2016 Biblical Studies Press, L. L. C.; [permissions](https://netbible.com/copyright/) |
| KJV | Public domain outside the United Kingdom; Crown printing rights apply in the UK. Courtesy of the Crosswire Bible Society and eBible.org |
| KJV+ | Public domain outside the United Kingdom; Crown printing rights apply in the UK. Courtesy of the Crosswire Bible Society and eBible.org |
| OJB | Copyright © 2002–2011, 2021, 2024 Artists for Israel International; [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Include copyright/source information and indicate changes |
| YLT | Public domain; translation by Robert Young. Courtesy of eBible.org |
| ASV | Public domain. Courtesy of eBible.org |
| BSB | Public domain; contributor BSB Publishing, LLC. Sources: [berean.bible](https://berean.bible/) and eBible.org |

The NET source is labeled **NET Bible**, with the archive's **1996–2016** copyright
notice. `eng-kjv` uses **KJV+** throughout the catalog and reader, distinct from
`eng-kjv2006` (**KJV**).

## Edition identity and structure

Stable source IDs come from consistent internal numbered book-file suffixes,
cross-checked against an eBible source link in `copr.htm` when present. Renaming
the outer ZIP does not change that identity. Without a source ID, the importer
uses `usfm-` followed by the first 32 hexadecimal SHA-256 digits of the UTF-8
edition title. The title comes from `copr.htm` or a consistent `\id` description.
Missing or conflicting metadata produces an error.

Book identity uses `\id` codes and the stable 80-entry `BibleBooks` registry.
Display names and aliases also retain source table-of-contents metadata. Books
follow source file ordering; chapter and verse lists come from actual source keys.
Partial collections and omitted numbers are valid. Supported chapter/verse markers
are ascending integers from 1 through 999; bridges and suffix labels are rejected
with an explicit error.

KJV+ contains **80 Scripture books**, with Apocrypha before Matthew. `FRT` and
`INT` are source front/introduction material, not reader books. Greek Esther
(`ESG`, stable ID 69) retains chapters **10–16**. OJB retains its source book order
and searchable `toc` names alongside canonical names and USFM codes.

## Installation and personal data

Download checks and USFM import are separate stages with separate status/errors.
Catalog transfers can be canceled and retried. Only a validated import registers
text in the shared library and activates it in the requesting reader. Local ZIP,
`.usfm`, and `.sfm` inputs use the same parser. The source file is unnecessary for
reading after successful import.

Annotations and reading state belong to the stable source ID. Same-ID reimport
preserves them; removal can retain them for later reimport. Windows share library
and annotation commits safely while selecting editions independently. Backups
contain active-edition personal data and source identity; restore requires the
matching installed, active edition and valid endpoints. Global preferences remain
local and are excluded from backups.

See [USFM.md](USFM.md) for parsing/storage details and
[tests/README.md](tests/README.md) for isolated checks of all seven source archives.
