#include "window.h"

#include "app/convert.h"

#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include <string>
#include <thread>
#include <vector>

namespace cue::converter {

namespace {

// The Cue app's colours (src/gui/theme.h).
const QColor bg{0x0A, 0x0A, 0x0B};
const QColor panel{0x11, 0x11, 0x13};
const QColor border{0x2A, 0x2A, 0x2E};
const QColor border2{0x3A, 0x3A, 0x40};
const QColor text{0xF4, 0xF4, 0xF5};
const QColor muted{0xA1, 0xA1, 0xAA};
const QColor dim{0x71, 0x71, 0x7A};
const QColor orange{0xFF, 0x8A, 0x1F};
const QColor green{0x4A, 0xDE, 0x80};
const QColor purple{0xC4, 0xB5, 0xFD};
const QColor red{0xF8, 0x71, 0x71};

QString css(const QColor& c) { return c.name(); }

bool convertible(const QString& path) {
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "rpp" || suffix == "cue" || suffix == "ini";
}

QString firstFile(const QMimeData* mime) {
    if (!mime || !mime->hasUrls())
        return {};
    for (const QUrl& url : mime->urls())
        if (url.isLocalFile() && convertible(url.toLocalFile()))
            return url.toLocalFile();
    return {};
}

} // namespace

DropZone::DropZone(QWidget* parent) : QWidget(parent) {
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(170);
}

void DropZone::setBusy(bool busy) {
    busy_ = busy;
    setCursor(busy ? Qt::BusyCursor : Qt::PointingHandCursor);
    update();
}

void DropZone::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    p.setBrush(hover_ ? QColor(0x1F, 0x14, 0x0A) : panel);
    QPen pen(hover_ || busy_ ? orange : border2, 1.5, Qt::DashLine);
    pen.setDashPattern({6, 4});
    p.setPen(pen);
    p.drawRoundedRect(r, 14, 14);

    // Two files and the arrows between them.
    const double h = height(), w = width();
    const double cy = h * 0.36, s = std::min(h * 0.22, 40.0);
    auto file = [&](double cx, const QString& label, const QColor& c) {
        QPainterPath path;
        const QRectF f(cx - s * 0.4, cy - s * 0.5, s * 0.8, s);
        const double fold = s * 0.25;
        path.moveTo(f.left(), f.top());
        path.lineTo(f.right() - fold, f.top());
        path.lineTo(f.right(), f.top() + fold);
        path.lineTo(f.right(), f.bottom());
        path.lineTo(f.left(), f.bottom());
        path.closeSubpath();
        p.setPen(QPen(c, 1.6));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        QFont font = p.font();
        font.setPixelSize(static_cast<int>(s * 0.24));
        font.setBold(true);
        p.setFont(font);
        p.drawText(QRectF(f.left() - 10, f.top() + s * 0.35, f.width() + 20, s * 0.5), Qt::AlignCenter, label);
    };
    file(w / 2 - s * 1.3, "RPP", text);
    file(w / 2 + s * 1.3, "CUE", orange);
    p.setPen(QPen(busy_ ? orange : muted, 1.6, Qt::SolidLine, Qt::RoundCap));
    const double ax = s * 0.55;
    p.drawLine(QPointF(w / 2 - ax, cy - 5), QPointF(w / 2 + ax, cy - 5));
    p.drawLine(QPointF(w / 2 + ax - 5, cy - 10), QPointF(w / 2 + ax, cy - 5));
    p.drawLine(QPointF(w / 2 - ax, cy + 5), QPointF(w / 2 + ax, cy + 5));
    p.drawLine(QPointF(w / 2 - ax + 5, cy + 10), QPointF(w / 2 - ax, cy + 5));

    QFont big = font();
    big.setPixelSize(17);
    big.setBold(true);
    p.setFont(big);
    p.setPen(text);
    p.drawText(QRectF(0, h * 0.62, w, 24), Qt::AlignCenter, busy_ ? "Converting…" : "Drop a .RPP or .cue file here");
    QFont small = font();
    small.setPixelSize(13);
    p.setFont(small);
    p.setPen(muted);
    p.drawText(QRectF(0, h * 0.62 + 26, w, 20), Qt::AlignCenter, busy_ ? "Reading the plug-ins' settings" : "or click to choose one");
}

void DropZone::mousePressEvent(QMouseEvent*) {
    if (busy_)
        return;
    const QString path = QFileDialog::getOpenFileName(this, "Convert a file", QString(),
                                                      "REAPER projects and Cue performances (*.rpp *.RPP *.cue);;Cue configs (*.ini)");
    if (!path.isEmpty())
        emit fileChosen(path);
}

void DropZone::dragEnterEvent(QDragEnterEvent* e) {
    if (!busy_ && !firstFile(e->mimeData()).isEmpty()) {
        e->acceptProposedAction();
        hover_ = true;
        update();
    }
}

void DropZone::dragLeaveEvent(QDragLeaveEvent*) {
    hover_ = false;
    update();
}

void DropZone::dropEvent(QDropEvent* e) {
    hover_ = false;
    update();
    const QString path = firstFile(e->mimeData());
    if (!path.isEmpty()) {
        e->acceptProposedAction();
        emit fileChosen(path);
    }
}

Window::Window(QWidget* parent) : QWidget(parent) {
    setWindowTitle("Cue Converter");
    setMinimumSize(520, 520);
    resize(600, 600);
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, bg);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, panel);
    pal.setColor(QPalette::Text, text);
    setPalette(pal);
    setStyleSheet(QString("QWidget { color: %1; }"
                          "QPushButton { background: %2; border: 1px solid %3; border-radius: 8px; padding: 8px 16px; font-weight: 600; }"
                          "QPushButton:hover { border-color: %4; color: %4; }"
                          "QListWidget { background: %2; border: 1px solid %3; border-radius: 10px; padding: 6px; }"
                          "QListWidget::item { padding: 4px 2px; }")
                      .arg(css(text), css(panel), css(border), css(orange)));

    auto* title = new QLabel(QString("<span style='color:%1'>Cue</span> Converter").arg(css(orange)));
    QFont tf = title->font();
    tf.setPixelSize(26);
    tf.setBold(true);
    title->setFont(tf);
    auto* subtitle = new QLabel("REAPER projects to Cue performances, and back. The new file goes beside the old one.");
    subtitle->setWordWrap(true);
    subtitle->setStyleSheet(QString("color: %1;").arg(css(muted)));

    drop_ = new DropZone;
    status_ = new QLabel;
    QFont sf = status_->font();
    sf.setPixelSize(16);
    sf.setBold(true);
    status_->setFont(sf);
    status_->setWordWrap(true);
    detail_ = new QLabel;
    detail_->setWordWrap(true);
    detail_->setStyleSheet(QString("color: %1;").arg(css(muted)));
    notes_ = new QListWidget;
    notes_->setWordWrap(true);
    notes_->setSelectionMode(QAbstractItemView::NoSelection);
    notes_->setFocusPolicy(Qt::NoFocus);
    open_ = new QPushButton("Show in folder");
    open_->setCursor(Qt::PointingHandCursor);
    connect(open_, &QPushButton::clicked, this, [this] {
        if (!lastOut_.isEmpty())
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(lastOut_).absolutePath()));
    });

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(open_);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 22);
    layout->setSpacing(10);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addSpacing(6);
    layout->addWidget(drop_, 3);
    layout->addSpacing(4);
    layout->addWidget(status_);
    layout->addWidget(detail_);
    layout->addWidget(notes_, 2);
    layout->addLayout(buttons);

    status_->hide();
    detail_->hide();
    notes_->hide();
    open_->hide();
    connect(drop_, &DropZone::fileChosen, this, &Window::convert);
}

void Window::convert(const QString& path) {
    drop_->setBusy(true);
    status_->hide();
    detail_->hide();
    notes_->hide();
    open_->hide();
    const std::string in = path.toStdString();
    // Finding plug-ins can open many files: off the window's thread.
    std::thread([this, in] {
        ConvertResult r;
        std::string err;
        const bool ok = convertFile(in, "", ConvertOptions{}, r, &err);
        QString summary;
        QStringList notes;
        if (ok) {
            summary = QString("%1 chain(s), %2 plug-in(s)").arg(r.chains).arg(r.plugins);
            if (r.skipped)
                summary += QString(", %1 left out").arg(r.skipped);
            if (r.missing)
                summary += QString(", %1 not found on this computer").arg(r.missing);
            bool pedalsLost = false;
            for (const auto& n : r.notes) {
                notes << QString::fromStdString(n);
                pedalsLost = pedalsLost || (n.rfind("ReaLearn:", 0) == 0 && n.find("not converted") != std::string::npos);
            }
            if (pedalsLost)
                summary += ". Some ReaLearn pedals were not converted: see the list";
        } else {
            summary = QString::fromStdString(err);
        }
        const QString out = QString::fromStdString(r.out);
        QMetaObject::invokeMethod(
            this, [this, ok, in, out, summary, notes] { showResult(ok, QString::fromStdString(in), out, summary, notes); }, Qt::QueuedConnection);
    }).detach();
}

void Window::showResult(bool ok, const QString& in, const QString& out, const QString& summary, const QStringList& notes) {
    drop_->setBusy(false);
    lastOut_ = ok ? out : QString();
    status_->setText(ok ? QString("✓  Wrote %1").arg(QFileInfo(out).fileName()) : QString("Could not convert %1").arg(QFileInfo(in).fileName()));
    status_->setStyleSheet(QString("color: %1;").arg(css(ok ? green : red)));
    detail_->setText(summary);
    notes_->clear();
    for (const QString& n : notes) {
        auto* item = new QListWidgetItem("•  " + n);
        item->setForeground(n.startsWith("left out") || n.startsWith("not found") ? purple : QColor(0xD4, 0xD4, 0xD8));
        notes_->addItem(item);
    }
    status_->show();
    detail_->show();
    notes_->setVisible(!notes.isEmpty());
    open_->setVisible(ok);
    emit finished(ok);
}

} // namespace cue::converter
