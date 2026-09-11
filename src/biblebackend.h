#pragma once

#include "epubimporter.h"
#include <QObject>
#include <QFutureWatcher>
#include <QSqlDatabase>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

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
    Q_PROPERTY(QVariantList books READ books CONSTANT)
    Q_PROPERTY(QVariantList collection READ collection NOTIFY collectionChanged)
    Q_PROPERTY(QString noteText READ noteText WRITE setNoteText NOTIFY noteTextChanged)
    Q_PROPERTY(bool selectionBookmarked READ selectionBookmarked NOTIFY annotationChanged)
    Q_PROPERTY(bool selectionHighlighted READ selectionHighlighted NOTIFY annotationChanged)
    Q_PROPERTY(double readingPosition READ readingPosition WRITE setReadingPosition NOTIFY readingPositionChanged)
    Q_PROPERTY(bool showVerseNumbers READ showVerseNumbers WRITE setShowVerseNumbers NOTIFY preferencesChanged)
    Q_PROPERTY(bool showHeadings READ showHeadings WRITE setShowHeadings NOTIFY preferencesChanged)
    Q_PROPERTY(bool versePicker READ versePicker WRITE setVersePicker NOTIFY preferencesChanged)
    Q_PROPERTY(double lineHeight READ lineHeight WRITE setLineHeight NOTIFY preferencesChanged)
    Q_PROPERTY(bool focusMode READ focusMode WRITE setFocusMode NOTIFY preferencesChanged)
    Q_PROPERTY(bool blockMode READ blockMode WRITE setBlockMode NOTIFY preferencesChanged)

public:
    explicit BibleBackend(QObject *parent = nullptr);
    ~BibleBackend() override;
    bool ready() const { return m_ready; }
    bool busy() const { return m_busy; }
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
    int book() const { return m_book; }
    int chapter() const { return m_chapter; }
    int selectedStart() const { return m_start; }
    int selectedEnd() const { return m_end; }
    int cursorVerse() const;
    bool crossChapterSelection() const;
    int chapterCount() const;
    QVariantList verses() const { return m_verses; }
    QVariantList books() const;
    QVariantList collection() const { return m_collection; }
    QString noteText() const { return m_note; }
    bool selectionBookmarked() const { return m_bookmarked; }
    bool selectionHighlighted() const { return m_highlighted; }
    double readingPosition() const { return m_position; }
    void setReadingPosition(double position);
    bool showVerseNumbers() const { return m_showVerseNumbers; }
    bool showHeadings() const { return m_showHeadings; }
    bool versePicker() const { return m_versePicker; }
    double lineHeight() const { return m_lineHeight; }
    bool focusMode() const { return m_focusMode; }
    bool blockMode() const { return m_blockMode; }
    Q_INVOKABLE void setShowVerseNumbers(bool value);
    Q_INVOKABLE void setShowHeadings(bool value);
    Q_INVOKABLE void setVersePicker(bool value);
    Q_INVOKABLE void setLineHeight(double value);
    Q_INVOKABLE void setFocusMode(bool value);
    Q_INVOKABLE void setBlockMode(bool value);

    Q_INVOKABLE void importEpub(QUrl file);
    Q_INVOKABLE bool switchVersion(QString id);
    Q_INVOKABLE bool removeVersion(QString id, bool removeAnnotations);
    Q_INVOKABLE void clearError();
    Q_INVOKABLE bool navigate(QString reference);
    // Pure preview: {valid, reference, error}; uses the current book for relative input.
    Q_INVOKABLE QVariantMap resolveReference(QString reference) const;
    Q_INVOKABLE QVariantList chapterVerses(int book, int chapter) const;
    Q_INVOKABLE void openChapter(int book, int chapter);
    Q_INVOKABLE void previousChapter();
    Q_INVOKABLE void nextChapter();
    Q_INVOKABLE void selectVerses(int start, int end);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void moveSelection(int delta, bool extend = false);
    // With no selection, start at the first verse and apply delta within this chapter.
    // Multi-verse steps stop at its edge; a subsequent step crosses by one verse.
    Q_INVOKABLE void moveByVerses(int delta, bool extend = false);
    Q_INVOKABLE void toggleBookmark();
    Q_INVOKABLE void toggleHighlight();
    Q_INVOKABLE void setNoteText(QString text);
    Q_INVOKABLE bool deleteAnnotation(QString reference);
    Q_INVOKABLE QString copySelection(bool plainText = false);
    Q_INVOKABLE QString copyVerse(int number, bool plainText = false);
    Q_INVOKABLE void copyText(QString text);
    Q_INVOKABLE bool exportNotes(QUrl directory);
    Q_INVOKABLE bool exportBackup(QUrl file);
    Q_INVOKABLE bool importBackup(QUrl file);
    Q_INVOKABLE void refreshCollection();
    Q_INVOKABLE QString dataDirectory();

signals:
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
    void noteTextChanged();
    void readingPositionChanged();
    void navigationChanged();
    void annotationChanged();
    void preferencesChanged();

private:
    struct Annotation {
        // Inclusive indices in the active SQL verse keys; persisted as references.
        int start = 0, end = 0;
        bool bookmark = false, highlight = false;
        QString note;
    };
    bool initializeStore();
    bool loadPreferences();
    bool savePreference(const QString &key, const QVariant &value);
    QString copyRange(int first, int last, const QString &reference, bool plainText);
    struct Verse { int book = 0, chapter = 0, verse = 0; };
    const QVector<Verse> &canonVerses() const { return m_keys; }
    int absoluteVerse(int book, int chapter, int verse) const;
    Verse endpoint(int index) const { return m_keys.value(index); }
    bool changeVersion(const QString &id, const QString &importPath = {},
                       const QString &removedId = {}, bool deleteAnnotations = false, bool startup = false);
    QString installedPath(const QString &id) const;
    bool readVersions(QVariantList *versions);
    void closeBible();
    bool loadChapter();
    bool readChapter(int book, int chapter, QVariantList *rows);
    QVariantList collectionRows(const QVector<Annotation> &annotations) const;
    bool protectDestination(const QString &path);
    bool goTo(int book, int chapter, int start, int end);
    bool goToSelection(int book, int chapter, int anchor, int cursor, double position);
    bool parseReference(QString input, int *book, int *chapter, int *start, int *end, QString *error) const;
    void intersectSelection();
    bool commitAnnotations(const QVector<Annotation> &annotations);
    bool flagCovered(const QVector<Annotation> &annotations, bool highlight) const;
    QString rangeReference(int start, int end) const;
    bool validRange(int book, int chapter, int start, int end) const;
    bool verseExists(int book, int chapter, int verse) const;
    bool persistState(int book, int chapter, int start, int end, double position);
    void updateSelection();
    void updateAnnotations();
    bool loadAnnotations(QVector<Annotation> *annotations);
    bool saveAnnotations(const QVector<Annotation> &annotations);
    void toggleFlag(bool highlight);
    bool requireReady();
    bool setError(const QString &error);
    void setStatus(const QString &status);
    QString localPath(const QUrl &url);
    static QString reference(int book, int chapter, int start = 0, int end = 0);

    QString m_directory, m_error, m_status, m_note;
    QString m_versionId, m_edition, m_abbreviation, m_copyright, m_importTarget;
    QVariantList m_versions;
    QVector<Verse> m_keys{{0,0,0}};
    QString m_bibleConnection, m_storeConnection;
    QSqlDatabase m_bible, m_store;
    bool m_ready = false, m_busy = false, m_bookmarked = false, m_highlighted = false;
    int m_book = 1, m_chapter = 1, m_start = 0, m_end = 0;
    int m_anchor = 0, m_cursor = 0; // Directional canonical indices; zero means no selection.
    double m_position = 0, m_progress = 0;
    bool m_showVerseNumbers = true, m_showHeadings = true, m_versePicker = true, m_focusMode = false, m_blockMode = false;
    double m_lineHeight = 1.5;
    QVariantList m_verses, m_collection;
    QVector<Annotation> m_annotations;
    std::shared_ptr<std::atomic<double>> m_importProgress;
    QFutureWatcher<EpubImporter::Result> m_importWatcher;
    QTimer m_progressTimer;
};
