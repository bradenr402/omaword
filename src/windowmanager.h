#pragma once
#include "singleinstance.h"
#include <QList>
#include <QUrl>
class BibleBackend;
class Desktop;
class QQmlApplicationEngine;
class QWindow;

class WindowManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool available READ available CONSTANT)
public:
    explicit WindowManager(Desktop *desktop, QObject *parent = nullptr,
                           QUrl source = QUrl(QStringLiteral("qrc:/qml/Main.qml")));
    ~WindowManager() override;
    bool available() const { return true; }
    int count() const { return m_readers.size(); }
    void openRequest(const SingleInstance::Request &request, SingleInstance::Reply reply, bool activate = true);
    QWindow *windowAt(int index) const;
    BibleBackend *backendAt(int index) const;
public slots:
    void newWindow();
signals:
    void countChanged();
private:
    struct Reader { QQmlApplicationEngine *engine = nullptr; BibleBackend *bible = nullptr; QWindow *window = nullptr; };
    Reader *createWindow();
    void retire(QWindow *window);
    static bool flush(QObject *root);
    Desktop *m_desktop;
    QUrl m_source;
    QList<Reader> m_readers;
    int m_serial = 0;
};
