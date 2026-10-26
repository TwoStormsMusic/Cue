#pragma once

// The converter's one window: drop a REAPER project or a .cue performance on
// it (or click to choose one) and it writes the other beside it, then says
// what it wrote and what it left out. Black with orange like the Cue app;
// green for a finished conversion, purple for the notes.

#include <QString>
#include <QStringList>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;

namespace cue::converter {

class DropZone : public QWidget {
    Q_OBJECT
public:
    explicit DropZone(QWidget* parent = nullptr);
    void setBusy(bool busy);

signals:
    void fileChosen(const QString& path);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    bool hover_ = false, busy_ = false;
};

class Window : public QWidget {
    Q_OBJECT
public:
    explicit Window(QWidget* parent = nullptr);
    // Converts path, as a drop would; the result shows when it is done.
    void convert(const QString& path);

signals:
    void finished(bool ok);

private:
    void showResult(bool ok, const QString& in, const QString& out, const QString& summary, const QStringList& notes);

    DropZone* drop_;
    QLabel* status_;
    QLabel* detail_;
    QListWidget* notes_;
    QPushButton* open_;
    QString lastOut_;
};

} // namespace cue::converter
