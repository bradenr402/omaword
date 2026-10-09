#pragma once

#include "scripturesearch.h"
#include "usfmimporter.h"
#include <QObject>
#include <QFutureWatcher>
#include <QHash>
#include <QSqlDatabase>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QSet>
#include <functional>
#include <optional>

class QJsonObject;

class BibleBackend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double importProgress READ importProgress NOTIFY importProgressChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString passage READ passage NOTIFY passageChanged)
    Q_PROPERTY(QString selectionReference READ selectionReference NOTIFY selectionChanged)
    Q_PROPERTY(QString edition READ edition NOTIFY versionChanged)
    Q_PROPERTY(QString versionId READ versionId NOTIFY versionChanged)
    Q_PROPERTY(QString versionAbbreviation READ versionAbbreviation NOTIFY versionChanged)
    Q_PROPERTY(QString versionCopyright READ versionCopyright NOTIFY versionChanged)
    Q_PROPERTY(QVariantList versions READ versions NOTIFY versionsChanged)
    Q_PROPERTY(int book READ book NOTIFY passageChanged)
    Q_PROPERTY(int chapter READ chapter NOTIFY passageChanged)
    Q_PROPERTY(int selectedStart READ selectedStart NOTIFY selectionChanged)
    Q_PROPERTY(int selectedEnd READ selectedEnd NOTIFY selectionChanged)
    Q_PROPERTY(int cursorVerse READ cursorVerse NOTIFY selectionChanged)
    Q_PROPERTY(bool crossChapterSelection READ crossChapterSelection NOTIFY selectionChanged)
    Q_PROPERTY(int chapterCount READ chapterCount NOTIFY passageChanged)
    Q_PROPERTY(QVariantList verses READ verses NOTIFY versesChanged)
    Q_PROPERTY(QVariantList books READ books NOTIFY booksChanged)
    Q_PROPERTY(QVariantList chapterNumbers READ chapterNumbers NOTIFY passageChanged)
    Q_PROPERTY(QVariantList collection READ collection NOTIFY collectionChanged)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY tagsChanged)
    Q_PROPERTY(QVariantList selectionTags READ selectionTags NOTIFY tagsChanged)
    Q_PROPERTY(QVariantList overlappingTags READ overlappingTags NOTIFY tagsChanged)
    Q_PROPERTY(QString noteText READ noteText WRITE setNoteText NOTIFY noteTextChanged)
    Q_PROPERTY(bool selectionHighlighted READ selectionHighlighted NOTIFY annotationChanged)
    Q_PROPERTY(double readingPosition READ readingPosition WRITE setReadingPosition NOTIFY readingPositionChanged)
    Q_PROPERTY(bool showVerseNumbers READ showVerseNumbers WRITE setShowVerseNumbers NOTIFY preferencesChanged)
    Q_PROPERTY(bool showHeadings READ showHeadings WRITE setShowHeadings NOTIFY preferencesChanged)
    Q_PROPERTY(double lineHeight READ lineHeight WRITE setLineHeight NOTIFY preferencesChanged)
    Q_PROPERTY(bool focusMode READ focusMode WRITE setFocusMode NOTIFY preferencesChanged)
    // Color \wj spans (format only); off by default, a shared preference.
    Q_PROPERTY(bool wordsOfJesus READ wordsOfJesus WRITE setWordsOfJesus NOTIFY preferencesChanged)
    // Margins (off by default, shared preferences): left chapter outline, right margin notes.
    Q_PROPERTY(bool chapterOutline READ chapterOutline WRITE setChapterOutline NOTIFY preferencesChanged)
    Q_PROPERTY(bool marginNotes READ marginNotes WRITE setMarginNotes NOTIFY preferencesChanged)
    // Bible search: every installed version (the active one first), or the active one only.
    Q_PROPERTY(bool searchAllVersions READ searchAllVersions WRITE setSearchAllVersions NOTIFY preferencesChanged)
    Q_PROPERTY(bool searchRegex READ searchRegex WRITE setSearchRegex NOTIFY preferencesChanged)
    Q_PROPERTY(bool blockMode READ blockMode WRITE setBlockMode NOTIFY preferencesChanged)
    Q_PROPERTY(QString readingMode READ readingMode WRITE setReadingMode NOTIFY preferencesChanged)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY preferencesChanged)
    Q_PROPERTY(bool hasParagraphs READ hasParagraphs NOTIFY versionChanged)
    // Text DB schema of the active version (4 = notes and words of Jesus kept,
    // 5 = poetry line levels, 6 = parallel passage lines).
    Q_PROPERTY(int textSchema READ textSchema NOTIFY versionChanged)
    Q_PROPERTY(bool hasWordsOfJesus READ hasWordsOfJesus NOTIFY versionChanged)
    Q_PROPERTY(bool hasCrossReferences READ hasCrossReferences NOTIFY versionChanged)
    // Any section heading anywhere in the active version.
    Q_PROPERTY(bool hasHeadings READ hasHeadings NOTIFY versionChanged)
    // Current chapter's footnotes {verse,seq,kind,caller,offset,text}, source order.
    Q_PROPERTY(QVariantList footnotes READ footnotes NOTIFY versesChanged)
    Q_PROPERTY(QVariantList searchResults READ searchResults NOTIFY searchChanged)
    Q_PROPERTY(bool searchBusy READ searchBusy NOTIFY searchChanged)
    Q_PROPERTY(QString searchError READ searchError NOTIFY searchChanged)
    Q_PROPERTY(int searchResultCount READ searchResultCount NOTIFY searchChanged)
    // The results' books in Bible order: {book, name, count, index} (index:
    // the book's first result among all of them).
    Q_PROPERTY(QVariantList searchBooks READ searchBooks NOTIFY searchChanged)
    Q_PROPERTY(QString textGeneration READ textGeneration NOTIFY versionChanged)
    Q_PROPERTY(QString deskPage READ deskPage NOTIFY deskChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY historyChanged)
    Q_PROPERTY(QStringList recentQueries READ recentQueries NOTIFY historyChanged)
    Q_PROPERTY(bool historyEnabled READ historyEnabled NOTIFY historyChanged)
    Q_PROPERTY(QVariantMap ribbon READ ribbon NOTIFY ribbonChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoChanged)
    Q_PROPERTY(QString undoLabel READ undoLabel NOTIFY undoChanged)
    Q_PROPERTY(QString redoLabel READ redoLabel NOTIFY undoChanged)

public:
    explicit BibleBackend(QObject *parent = nullptr);
    ~BibleBackend() override;
    bool ready() const { return m_ready; }
    bool busy() const;
    // Window roots register synchronous draft saves; false vetoes shared replacement.
    void setFlushCallback(std::function<bool()> callback) { m_flush = std::move(callback); }
    double importProgress() const { return m_progress; }
    QString error() const { return m_error; }
    QString status() const { return m_status; }
    QString passage() const;
    QString selectionReference() const;
    QString edition() const { return m_edition; }
    QString versionId() const { return m_versionId; }
    QString versionAbbreviation() const { return m_abbreviation; }
    QString versionCopyright() const { return m_copyright; }
    QVariantList versions() const { return m_versions; }
    // The installed version a name means, by ID or abbreviation in any case
    // ("" for none): the command line's --translation.
    QString versionIdFor(const QString &name) const;
    // Reads another installed version in this backend alone (the command
    // line's --translation, the lookup window's version choice): unlike
    // switchVersion, it records neither the version as the one you read nor
    // a place in it.
    Q_INVOKABLE bool readVersion(const QString &id);
    int book() const { return m_book; }
    int chapter() const { return m_chapter; }
    int selectedStart() const { return m_start; }
    int selectedEnd() const { return m_end; }
    int cursorVerse() const;
    bool crossChapterSelection() const;
    int chapterCount() const;
    QVariantList chapterNumbers() const { return chaptersForBook(m_book); }
    Q_INVOKABLE QVariantList chaptersForBook(int book) const;
    QVariantList verses() const { return m_verses; }
    QVariantList books() const;
    QVariantList collection() const { return m_collection; }
    QVariantList tags() const;
    QVariantList selectionTags();
    QVariantList overlappingTags();
    // Capture once when opening an editor; pass this value on every mutation.
    // Identity is versionId + all six endpoints. reference is display-only.
    // Exact queries return {id,name,count}; overlap/browser queries return ranges.
    // tagsChanged is delivered to every window after committed tag/library changes,
    // and locally on selection changes (the exact and overlapping properties).
    Q_INVOKABLE QVariantMap tagScope() const;
    Q_INVOKABLE QVariantList tagsForScope(QVariantMap scope, bool overlapping = false);
    Q_INVOKABLE QString createTag(QString name);
    Q_INVOKABLE bool setTag(QVariantMap scope, QString tagId, bool assigned);
    // Several tags on one range in one transaction (one undo step).
    Q_INVOKABLE bool setTags(QVariantMap scope, QVariantList tagIds, bool assigned);
    Q_INVOKABLE QString createAndAssignTag(QVariantMap scope, QString name);
    Q_INVOKABLE bool renameTag(QString id, QString name);
    Q_INVOKABLE bool deleteTag(QString id);
    Q_INVOKABLE bool mergeTags(QString sourceId, QString targetId);
    // Empty filters produce no results; empty versionId searches installed editions.
    // Results contain all tags, exact marks/note, bounded excerpt and sharedCount.
    // Querying never navigates; openTaggedPassage is the explicit navigation action.
    Q_INVOKABLE QVariantList taggedPassages(QVariantList tagIds, bool matchAll = false, QString versionId = QString());
    Q_INVOKABLE QVariantList relatedPassages(QVariantMap source, QVariantList tagIds,
                                           bool matchAll = false, bool allVersions = false);
    Q_INVOKABLE bool openTaggedPassage(QVariantMap scope);
    // Snapshot: {ok,error,scope,highlight,note,tags:[{id,name}],token}.
    // Tokens fingerprint all exact components and the installed Bible generation.
    Q_INVOKABLE QVariantMap annotationSnapshot(QVariantMap scope);
    // components: {highlight:bool,note:bool,tags:[ids]}.
    // Pure preview: {ok,error,needsNoteMerge,sourceNote,destinationNote,
    // destinationToken,combinedNote,message}. Combined order is destination,
    // two newlines, then source. Equal notes are deduplicated without confirmation.
    Q_INVOKABLE QVariantMap previewRangeChange(QVariantMap sourceSnapshot, QVariantMap destination,
                                             QVariantMap components);
    Q_INVOKABLE bool changeAnnotationRange(QVariantMap sourceSnapshot, QVariantMap destination,
                                          QVariantMap components, bool combineNotes = false,
                                          QString destinationToken = QString());
    // Marks: every exact range with a highlight, a note, or tags, in every
    // installed version, one row each: canonical scope fields + reference,
    // versionAbbreviation, active (the reading version), highlight, note,
    // tags [{id,name}] in name order, created/updated (the annotation's;
    // empty when unknown or tags only), and excerpt (bounded verse text).
    // Ordered by passage (the reading version's book order, then chapter and
    // verse), the reading version first among equal ranges. Read on demand;
    // marksChanged says when to read again.
    Q_INVOKABLE QVariantList marks() const;
    // The saved note on an exact range of any installed version ("" when none).
    Q_INVOKABLE QString savedNote(QVariantMap scope) const;
    // Saves text as an exact range's note in any installed version, only while
    // its saved note still equals expected (else the "changed in another
    // window" error). A range left with nothing goes. Successive saves on one
    // range are one undo step until closeUndoGroup(), as with setNoteText.
    Q_INVOKABLE bool setScopeNote(QVariantMap scope, QString text, QString expected);
    // Local reading-only restoration in the active version; never flushes peer drafts.
    // An unselected chapter uses {versionId,book,chapter,start:0,end:0}.
    Q_INVOKABLE bool restoreSelection(QVariantMap scope, bool reversed = false, double position = -1);
    QString noteText() const { return m_note; }
    bool selectionHighlighted() const { return m_highlighted; }
    double readingPosition() const { return m_position; }
    void setReadingPosition(double position);
    bool showVerseNumbers() const { return m_showVerseNumbers; }
    bool showHeadings() const { return m_showHeadings; }
    double lineHeight() const { return m_lineHeight; }
    bool focusMode() const { return m_focusMode; }
    bool wordsOfJesus() const { return m_wordsOfJesus; }
    bool chapterOutline() const { return m_chapterOutline; }
    bool marginNotes() const { return m_marginNotes; }
    bool searchAllVersions() const { return m_searchAllVersions; }
    // Bible search reads every query as a regular expression (alt r);
    // otherwise only a query in slashes is one.
    bool searchRegex() const { return m_searchRegex; }
    // Compatibility setter: true -> block, false -> verse. Default paragraph
    // mode also reports false; callers should use readingMode for three states.
    bool blockMode() const { return m_readingMode == "block"; }
    QString readingMode() const { return m_readingMode; }
    int fontSize() const { return m_fontSize; }
    bool hasParagraphs() const { return m_hasParagraphs; }
    static constexpr int CurrentTextSchema = 6;
    // Below this the text lacks what every version shows (footnotes, poetry
    // indents), so Versions offers a re-import; schema 6 adds only parallel
    // passages, which few sources have (the catalog says which).
    static constexpr int ReimportTextSchema = 5;
    int textSchema() const { return m_textSchema; }
    bool hasWordsOfJesus() const { return m_hasWordsOfJesus; }
    bool hasCrossReferences() const { return m_hasCrossReferences; }
    bool hasHeadings() const { return m_hasHeadings; }
    QVariantList footnotes() const;
    // Records: versionId/versionAbbreviation, book/chapter/start/endBook/
    // endChapter/end, reference, source-preserving text, score (exact-term count),
    // segments[{verse,start,length}], displaySegments[{start,length}]. Segment
    // positions are UTF-16; start/end endpoints are actual source verse IDs.
    QVariantList searchResults() const { return m_searchResults; }
    bool searchBusy() const { return m_searchBusy; }
    QString searchError() const { return m_searchError; }
    int searchResultCount() const { return int(m_allSearchResults.size()); }
    QVariantList searchBooks() const { return m_searchBooks; }
    QString textGeneration() const { return m_textGeneration; }
    // Bible syntax: AND substrings in one verse, quoted phrases across adjacent
    // source verses in a chapter. Quotes/backslashes can be backslash-escaped.
    // A query in slashes, or any with searchRegex, is a regular expression
    // (ScriptureSearch::Syntax).
    // Queries over 4096 UTF-16 units return an explicit error, never truncation.
    // With searchAllVersions, every installed version's matches follow the
    // active one's, each in Bible order; searchBooks then has one entry per
    // version and book {versionId, versionAbbreviation, book, name, count,
    // index}. A search index in the cache directory (ScriptureSearch::
    // searchEditions) finds the chapters to read.
    Q_INVOKABLE void searchScripture(QString query);
    // What a search reads: the version being read, then with `every` each
    // other installed version, in library order.
    QVector<ScriptureSearch::Edition> searchEditions(bool every) const;
    Q_INVOKABLE void cancelScriptureSearch();
    Q_INVOKABLE void loadMoreSearchResults();
    // Pure cached-chapter scan; source UTF-16 segments, one semantic range per
    // Bible match, individual occurrences for literal chapter find.
    Q_INVOKABLE QVariantMap findInChapter(QString query, bool bibleSyntax = false) const;
    // Chapter find (`/`): a smart-case regular expression (ScriptureSearch::findPattern).
    Q_INVOKABLE QVariantMap findPatternInChapter(QString pattern) const;
    Q_INVOKABLE void setShowVerseNumbers(bool value);
    Q_INVOKABLE void setShowHeadings(bool value);
    Q_INVOKABLE void setLineHeight(double value);
    Q_INVOKABLE void setFocusMode(bool value);
    Q_INVOKABLE void setWordsOfJesus(bool value);
    Q_INVOKABLE void setChapterOutline(bool value);
    Q_INVOKABLE void setMarginNotes(bool value);
    Q_INVOKABLE void setSearchAllVersions(bool value);
    Q_INVOKABLE void setSearchRegex(bool value);
    Q_INVOKABLE void setBlockMode(bool value);
    Q_INVOKABLE void setReadingMode(QString value);
    Q_INVOKABLE void setFontSize(int value);

    // The Desk: one global Markdown page in personal.sqlite (schema 10),
    // shared live by every window of this data directory and kept across
    // restarts. appendToDesk adds a scope's quotation at the end: the passage
    // laid out as copySelection() lays it out, as a Markdown blockquote,
    // followed by "— John 3:16 (NET)" (deskQuotation). setDeskPage replaces
    // the page only while it still reads `expected` (else the "changed in
    // another window" error). Backups (format 7) carry the page.
    QString deskPage() const { return m_deskPage; }
    // Empty scope means the current selection. Returns the scope's reference
    // ("" on failure, with the error set). Other windows' drafts save first.
    // An undo step: undo takes the quotation off while the page still reads
    // as the addition left it.
    Q_INVOKABLE QString appendToDesk(QVariantMap scope = QVariantMap());
    Q_INVOKABLE bool setDeskPage(QString text, QString expected);
    // Clean Desk: empties the page in every window, as an undo step.
    Q_INVOKABLE bool cleanDesk();
    // Copies any canonical scope (marker, annotation, search result) without navigating.
    Q_INVOKABLE QString copyScope(QVariantMap scope, bool plainText = false);

    // History: one global, most-recent-first list of visited passages in
    // personal.sqlite (schema 7), shared live by every window and never part of
    // backups. Identity is versionId + six endpoints; a chapter without a
    // selection uses start=end=0 with endBook/endChapter equal to book/chapter.
    // Recording an existing location moves it to the top (refreshing position
    // and visitedAt); the list keeps the newest HistoryLimit entries. Rows:
    // {id,versionId,versionAbbreviation,reference,book,chapter,start,endBook,
    // endChapter,end,position,visitedAt,available}; available chapter rows
    // also carry firstVerse/lastVerse so the whole chapter can be copied or added. Recent Bible search
    // queries (newest first, at most RecentQueryLimit) live beside the list and
    // are cleared with it. While disabled (a shared preference) nothing is
    // recorded; disabling clears both lists. Windows decide when a visit counts.
    static constexpr int HistoryLimit = 500;
    static constexpr int RecentQueryLimit = 5;
    QVariantList history() const { return m_history; }
    QStringList recentQueries() const { return m_recentQueries; }
    bool historyEnabled() const { return m_historyEnabled; }
    // {versionId,book,chapter,start,endBook,endChapter,end,position} for this window.
    Q_INVOKABLE QVariantMap currentLocation() const;
    // Empty location means currentLocation(). False (without an error) when disabled.
    Q_INVOKABLE bool recordHistory(QVariantMap location = QVariantMap());
    Q_INVOKABLE bool removeHistoryItem(int id);
    Q_INVOKABLE bool clearHistory();
    Q_INVOKABLE bool setHistoryEnabled(bool enabled);
    Q_INVOKABLE bool recordQuery(QString query);
    // Switches version when needed, then restores the range and reading position.
    Q_INVOKABLE bool openLocation(QVariantMap location);

    // The ribbon: one global reading place in personal.sqlite (schema 8),
    // shared live by every window, kept across restarts, never backed up.
    // placeRibbon() puts it at this window's currentLocation() (the selection,
    // else the chapter and its reading position); it stays there until placed
    // again or removed (removeRibbon(), for every window; nothing places it
    // again by itself). ribbon: {} or {versionId, book, chapter, start, endBook,
    // endChapter, end, position, placedAt, reference, chapterReference}, the
    // references in the active version's book names. ribbonLocation() is where
    // it opens: in the active version when that has the passage, else in the
    // version it was placed in, else its chapter here; {} when none has it.
    QVariantMap ribbon() const;
    Q_INVOKABLE bool placeRibbon();
    Q_INVOKABLE bool removeRibbon();
    Q_INVOKABLE QVariantMap ribbonLocation() const;

    // Undo: this window's own annotation changes (highlight, note, tag
    // assignment, deletion, range move), newest first, in memory only.
    // Each step keeps the exact saved state of every range it touched, before
    // and after: undo writes "before" back only while those ranges still read
    // "after" (redo the reverse), otherwise it fails with a "changed in another
    // window" error and the stale step is dropped. A new change clears redo.
    // Successive note saves on one range are one step until closeUndoGroup()
    // (the editor closing) or any other step. Labels read after "Undo"/"Redo":
    // "highlight on John 3:16". Desk additions are steps too (the page must
    // still read as they left it). Tag management, backups, version removal,
    // typing on the Desk, and History are not undoable.
    static constexpr int UndoLimit = 100;
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    QString undoLabel() const { return m_undo.isEmpty() ? QString() : m_undo.last().label; }
    QString redoLabel() const { return m_redo.isEmpty() ? QString() : m_redo.last().label; }
    Q_INVOKABLE bool undo() { return applyUndo(true); }
    Q_INVOKABLE bool redo() { return applyUndo(false); }
    Q_INVOKABLE void closeUndoGroup() { m_undoGroup.clear(); }

    // expectedId (Re-import) rejects a file of another edition before importing.
    Q_INVOKABLE void importUsfm(QUrl file, QString expectedId = QString());
    Q_INVOKABLE bool switchVersion(QString id);
    Q_INVOKABLE bool removeVersion(QString id, bool removeAnnotations);
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool navigate(QString reference);
    // Pure preview: {valid, reference, error}; uses the current book for relative input.
    Q_INVOKABLE QVariantMap resolveReference(QString reference) const;
    // Shared fuzzy parser (palette, note [[ suggestions): ranked candidates
    // {reference, book, chapter, start, end, score}, most similar first. Accepts
    // compact input (jn316, 1co13, ps23, rev 3 14-22) besides every exact form.
    Q_INVOKABLE QVariantList referenceCandidates(QString input, int limit = 8) const;
    // Exact plain-text passage links: a book name or standard abbreviation,
    // whitespace, then an existing chapter or chapter:verse(-verse). Compact
    // forms never link. Rows {start, length, text, reference} in UTF-16 offsets.
    Q_INVOKABLE QVariantList passageLinks(QString text) const;
    // Notes (other than the current exact range) whose links overlap the
    // current selection, or the chapter without one. Collection-shaped rows.
    Q_INVOKABLE QVariantList linkedNotes() const;
    // Cross references for a scope in the active version (empty: the current
    // selection; none without a verse): the source's own \x targets (schema 4)
    // in source order, then its parallel passages (schema 6: the \r lines of
    // the section the passage is in), then bundled OpenBible.info ones by
    // votes (KJV verse numbers), each range once, targets absent from this
    // version and the passage itself hidden.
    // Rows: canonical scope fields + reference, excerpt, source ("own",
    // "parallel", "openBible"), openBible, votes.
    Q_INVOKABLE QVariantList crossReferences(QVariantMap scope = QVariantMap()) const;
    // Every installed version's text for a scope's endpoints (empty: the
    // selection), in library order: {versionId, abbreviation, name, active,
    // available, reference, text}. text is laid out as copySelection(true);
    // available is false where a version lacks an endpoint (its numbering
    // differs, or it has no such book).
    Q_INVOKABLE QVariantList compareVersions(QVariantMap scope = QVariantMap()) const;
    Q_INVOKABLE QVariantList chapterVerses(int book, int chapter) const;
    // Chapter reads from the text database (tests: the next one is read ahead).
    Q_INVOKABLE int chapterReads() const { return m_chapterReads; }
    Q_INVOKABLE void openChapter(int book, int chapter);
    Q_INVOKABLE void previousChapter();
    Q_INVOKABLE void nextChapter();
    Q_INVOKABLE void selectVerses(int start, int end);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void moveSelection(int delta, bool extend = false);
    // With no selection, start at the first verse and apply delta within this chapter.
    // Multi-verse steps stop at its edge; a subsequent step crosses by one verse.
    Q_INVOKABLE void moveByVerses(int delta, bool extend = false);
    // True only after persistence succeeds; false must not complete Visual mode.
    Q_INVOKABLE bool toggleHighlight();
    Q_INVOKABLE void setNoteText(QString text);
    // Saved UI passes its captured range map; reference is display-only.
    // Deletes exact marks/note/tags in scope.versionId without navigating.
    Q_INVOKABLE bool deleteAnnotationScope(QVariantMap scope);
    Q_INVOKABLE bool deleteAnnotation(QString reference, QString expectedVersionId = QString());
    // Clears only the note on this exact range, keeping marks and tags; a row
    // left with no highlight or note is removed.
    Q_INVOKABLE bool deleteNoteScope(QVariantMap scope);
    Q_INVOKABLE QString copySelection(bool plainText = false);
    // The command line's passages (omaword quote, link): a reference's
    // passage in the version being read (a whole chapter for "John 3"),
    // without navigating, saving, or touching the clipboard: {reference,
    // versionId, abbreviation, name, plain, quotation (both laid out as
    // copySelection() lays them out), verses [{book, chapter, verse, text}]},
    // or {error}. Compact input (jn316) is read as the passage palette reads
    // it, unless strict.
    Q_INVOKABLE QVariantMap readPassage(const QString &text, bool strict = false);
    Q_INVOKABLE QString copyVerse(int number, bool plainText = false);
    Q_INVOKABLE void copyText(QString text);
    // Character selection over the current chapter's source text (see
    // textselection.h). Positions are {verse, offset}: a verse number and a
    // UTF-16 offset into its plain text.
    Q_INVOKABLE QVariantMap textMotion(QString motion, int verse, int offset, int count = 1) const;
    Q_INVOKABLE QVariantMap textObject(QString object, int anchorVerse, int anchorOffset,
                                       int cursorVerse, int cursorOffset, int count = 1) const;
    Q_INVOKABLE int characterEnd(int verse, int offset) const;
    // [start, end) with an exclusive end; quotation follows the ellipsis and
    // verse-number rules, plain is the selected text. Copy also sets the clipboard.
    Q_INVOKABLE QString characterText(int startVerse, int startOffset, int endVerse, int endOffset,
                                      bool quotation = true) const;
    Q_INVOKABLE QString copyCharacters(int startVerse, int startOffset, int endVerse, int endOffset,
                                       bool plainText = false);
    // The desktop's PRIMARY selection (middle-click paste), where supported.
    // lastPrimarySelection() is the last text offered (also where unsupported).
    Q_INVOKABLE void setPrimarySelection(QString text);
    Q_INVOKABLE QString lastPrimarySelection() const { return m_primarySelection; }
    Q_INVOKABLE bool exportNotes(QUrl directory);
    Q_INVOKABLE bool exportBackup(QUrl file);
    Q_INVOKABLE bool importBackup(QUrl file);
    Q_INVOKABLE void refreshCollection();
    Q_INVOKABLE QString dataDirectory();

signals:
    void searchChanged();
    void booksChanged();
    void versionChanged();
    void versionsChanged();
    void readyChanged();
    void busyChanged();
    void importProgressChanged();
    void errorChanged();
    void statusChanged();
    void passageChanged();
    void selectionChanged();
    void versesChanged();
    void collectionChanged();
    void tagsChanged();
    void noteTextChanged();
    void readingPositionChanged();
    void navigationChanged();
    void annotationChanged();
    void preferencesChanged();
    void deskChanged();
    void historyChanged();
    void undoChanged();
    void ribbonChanged();
    // Annotations or tags changed in some version (any window): marks() is stale.
    void marksChanged();
    void sharedCommitted(QString versionId, bool library, bool preferences);

private:
    struct Annotation {
        // Inclusive indices in the active SQL verse keys; persisted as references.
        int start = 0, end = 0;
        bool highlight = false;
        QString note;
        // UTC ISO 8601 with milliseconds; empty for rows saved before schema 9.
        QString created, updated;
    };
    bool initializeStore();
    bool migrateBookmarks();
    // Backup formats 6 and 7: every installed version's marks and reading
    // place, and the Desk (format 6 as passages, 7 as the page).
    bool restoreEveryVersion(const QJsonObject &root);
    // A backup's tag definitions by name: existing global identities are
    // reused (never renamed or deleted); ids maps each backup id to the store's.
    bool restoreTagDefinitions(const QMap<QString,QString> &definitions, QMap<QString,QString> *ids);
    bool loadPreferences();
    bool savePreference(const QString &key, const QVariant &value);
    QString copyRange(int first, int last, const QString &reference, bool plainText);
    struct Verse { int book = 0, chapter = 0, verse = 0; bool paragraphStart = false; };
    const QVector<Verse> &canonVerses() const { return m_keys; }
    int absoluteVerse(int book, int chapter, int verse) const;
    Verse endpoint(int index) const { return m_keys.value(index); }
    bool changeVersion(const QString &id, const QString &importPath = {},
                       const QString &removedId = {}, bool deleteAnnotations = false, bool startup = false,
                       bool peerReload = false, const QVariantMap &targetScope = {});
    void refreshShared(const QString &versionId, bool library, bool preferences);
    bool flushDrafts();
    // versionId: the version whose annotations changed (empty: the reading one).
    void publishAnnotations(const QString &versionId = QString());
    bool sharedMutationBusy() const;
    void notifyBusy();
    std::function<bool()> m_flush;
    bool m_mutating = false, m_flushing = false;
    QSet<QString> m_pendingAnnotations;
    QString installedPath(const QString &id) const;
    bool ownedBiblePath(const QString &path, const QString &id = {}) const;
    void removeOwnedBibleFiles(const QString &path, const QString &id = {});
    bool readVersions(QVariantList *versions);
    QPair<int, bool> textInfo(const QString &id, const QString &path) const;
    mutable QHash<QString, QPair<int, bool>> m_textInfo;
    void closeBible();
    bool loadChapter();
    bool readChapter(int book, int chapter, QVariantList *rows);
    void prefetchNextChapter();
    QPair<QPair<int,int>, QVariantList> m_prefetched;
    QString m_prefetchGeneration;
    QTimer m_prefetchTimer;
    int m_chapterReads = 0;
    QVariantList collectionRows(const QVector<Annotation> &annotations) const;
    bool protectDestination(const QString &path);
    bool goTo(int book, int chapter, int start, int end);
    bool goToSelection(int book, int chapter, int anchor, int cursor, double position);
    bool parseReference(QString input, int *book, int *chapter, int *start, int *end, QString *error) const;
    struct Link { int start = 0, length = 0, first = 0, last = 0; QString text, reference; };
    QVector<Link> findLinks(const QString &text) const;
    QVector<QPair<int, int>> sourceTargets(const QString &text, int book, int chapter) const;
    bool chapterBounds(int book, int chapter, int *first, int *last) const;
    void intersectSelection();
    bool commitAnnotations(const QVector<Annotation> &annotations, const QVector<Annotation> &before, const QString &label);
    bool highlightCovered(const QVector<Annotation> &annotations) const;
    QString rangeReference(int start, int end) const;
    bool validRange(int book, int chapter, int start, int end) const;
    bool verseExists(int book, int chapter, int verse) const;
    bool persistState(int book, int chapter, int start, int end, double position);
    void updateSelection();
    void updateAnnotations();
    bool loadAnnotations(QVector<Annotation> *annotations);
    bool saveAnnotations(const QVector<Annotation> &annotations);
    bool validateScope(const QVariantMap &scope, QVariantMap *canonical = nullptr) const;
    bool rangeEditScope(const QVariantMap &scope, QVariantMap *canonical) const;
    QVariantMap readRangeSnapshot(const QVariantMap &scope) const;
    QVariantMap prepareRangeChange(const QVariantMap &sourceSnapshot, const QVariantMap &destination,
                                   const QVariantMap &components, QVariantMap *source,
                                   QVariantMap *target) const;
    QVariantList tagRangeRows(const QString &versionId, bool excerpts) const;
    // created: the assignment's time when restoring one (else now).
    bool writeTagAssignment(const QVariantMap &scope, const QString &id, bool assigned,
                            const std::optional<QString> &created = std::nullopt);
    QString ensureTag(const QString &name);
    QString tagName(const QString &id) const;
    bool commitTagMutation();
    void publishTags();
    // One exact range's saved state: its annotation row (annotated false: no
    // highlight, no note) with its times, and its tag ids in SQLite order with
    // when each was assigned.
    struct ScopeState {
        bool annotated = false, highlight = false;
        QString note, created, updated;
        QStringList tags, tagCreated;
        bool operator==(const ScopeState &other) const {
            return annotated == other.annotated && highlight == other.highlight && note == other.note
                && created == other.created && updated == other.updated && tags == other.tags
                && tagCreated == other.tagCreated;
        }
        bool operator!=(const ScopeState &other) const { return !(*this == other); }
        // The same highlight, note, and tags, whenever they were saved.
        bool sameMarks(const ScopeState &other) const {
            return annotated == other.annotated && highlight == other.highlight && note == other.note && tags == other.tags;
        }
    };
    struct UndoStep {
        UndoStep() = default;
        UndoStep(QString label, QString versionId, QVector<QVariantMap> scopes, QString group = {})
            : label(std::move(label)), versionId(std::move(versionId)), group(std::move(group)), scopes(std::move(scopes)) {}
        QString label, versionId;
        // An open note session's range: its later saves extend this step.
        QString group;
        QVector<QVariantMap> scopes;
        QVector<ScopeState> before, after;
        QVector<QPair<QString, QString>> createdTags; // {id, name} the step created
        // A Desk addition: the page before and after it (no scopes).
        bool desk = false;
        QString deskBefore, deskAfter;
    };
    QVector<UndoStep> m_undo, m_redo;
    QString m_undoGroup;
    QVariantMap annotationScope(int start, int end) const;
    bool readScopeState(const QVariantMap &scope, ScopeState *state);
    bool writeScopeState(const QVariantMap &scope, const ScopeState &state);
    bool captureStates(UndoStep *step, bool after);
    void recordUndo(UndoStep step);
    void clearUndo();
    bool applyUndo(bool undo);
    bool migrateDeskPage();
    // A scope's Desk quotation (see deskPage); a version that is not
    // installed, or no longer has the passage, gives just its reference.
    QString deskQuotation(const QVariantMap &scope) const;
    // The page with each scope's quotation added at its end, a blank line apart.
    QString withQuotations(QString page, const QVariantList &scopes) const;
    bool writeDeskPage(const QString &text);
    void reloadDesk();
    void publishDesk();
    void reloadHistory();
    void publishHistory();
    void reloadRibbon();
    void publishRibbon();
    bool validLocation(const QVariantMap &location) const;
    bool requireReady();
    bool setError(const QString &error);
    void setStatus(const QString &status);
    QString localPath(const QUrl &url);
    QString reference(int book, int chapter, int start = 0, int end = 0) const;
    int resolveBook(QString name) const;

    QString m_directory, m_error, m_status, m_note;
    QString m_versionId, m_edition, m_abbreviation, m_copyright, m_importTarget;
    QString m_primarySelection;
    QVariantList m_versions;
    QVariantList m_books;
    QHash<int, QVariantList> m_chapters;
    QHash<QString, int> m_bookAliases;
    QHash<int, int> m_keyIndices;
    QVector<Verse> m_keys{{0,0,0}};
    QString m_bibleConnection, m_storeConnection;
    QSqlDatabase m_bible, m_store;
    bool m_storeReady = false;
    bool m_ready = false, m_busy = false, m_highlighted = false;
    int m_book = 1, m_chapter = 1, m_start = 0, m_end = 0;
    int m_anchor = 0, m_cursor = 0; // Directional canonical indices; zero means no selection.
    double m_position = 0, m_progress = 0;
    bool m_showVerseNumbers = true, m_showHeadings = true, m_focusMode = false, m_wordsOfJesus = false;
    bool m_chapterOutline = false, m_marginNotes = false, m_searchAllVersions = false, m_searchRegex = false;
    QString m_readingMode = "paragraph";
    int m_fontSize = 18;
    bool m_hasParagraphs = false, m_hasWordsOfJesus = false, m_hasHeadings = false, m_hasCrossReferences = false;
    int m_textSchema = 0;
    double m_lineHeight = 1.5;
    QVariantList m_verses, m_collection, m_history;
    QString m_deskPage;
    QVariantMap m_ribbon;
    QStringList m_recentQueries;
    bool m_historyEnabled = true;
    QVector<Annotation> m_annotations;
    std::shared_ptr<std::atomic<double>> m_importProgress;
    QFutureWatcher<UsfmImporter::Result> m_importWatcher;
    QTimer m_progressTimer;
    QString m_textGeneration, m_searchError;
    QVariantList m_searchResults, m_allSearchResults, m_searchBooks;
    bool m_searchBusy = false;
    quint64 m_searchRequest = 0;
    std::shared_ptr<std::atomic_bool> m_searchCancellation;
};
