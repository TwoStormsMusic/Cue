#include "gui/mainwindow.h"

#include "gui/chainview.h"
#include "gui/pluginsview.h"
#include "gui/settingsview.h"
#include "gui/stageview.h"
#include "gui/theme.h"
#include "gui/tunerview.h"

#include <QApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QPainter>
#include <QStackedLayout>

#include <algorithm>

namespace cue::gui {

namespace {

// The message line: drawn over the view, never in the way of a tap.
class Toast : public QWidget {
public:
    Toast(QWidget* parent, const QString& text, const bool& error) : QWidget(parent), text_(text), error_(error) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const double u = std::clamp(std::min(width() / 1280.0, parentWidget()->height() / 860.0), 0.4, 4.0);
        draw::card(p, rect(), 12 * u, error_ ? color::redBg : color::raised, error_ ? color::red : color::border2);
        p.setPen(error_ ? color::red : color::text);
        p.setFont(typeface(Face::Text, 16 * u, QFont::Medium));
        p.drawText(rect().adjusted(18 * u, 0, -18 * u, 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextWordWrap, text_);
    }

private:
    const QString& text_;
    const bool& error_;
};

} // namespace

MainWindow::MainWindow(AppState& state, QWidget* parent) : QWidget(parent), s_(state) {
    setWindowTitle("Cue");
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, color::bg);
    setPalette(pal);
    stack_ = new QStackedLayout(this);
    stack_->setContentsMargins(0, 0, 0, 0);
    views_.insert("stage", new StageView(s_, this));
    views_.insert("tuner", new TunerView(s_, this));
    auto* chains = new ChainView(s_, this);
    views_.insert("chains", chains);
    // The plug-ins view adds to the chain the chain view shows, then goes
    // back to it.
    auto* plugins = new PluginsView(s_, this);
    plugins->target = [chains] { return chains->addTarget(); };
    plugins->add = [this, chains](const PluginInfo& plugin) {
        chains->addFromBrowser(plugin);
        showView("chains");
    };
    views_.insert("plugins", plugins);
    auto* settings = new SettingsView(s_, this);
    views_.insert("settings", settings);
    for (const char* name : {"stage", "tuner", "chains", "plugins", "settings"}) {
        stack_->addWidget(views_[name]);
        connect(views_[name], &Surface::navigate, this, [this](const QString& v) {
            autoTuner_ = false;
            showView(v);
        });
    }
    connect(settings, &SettingsView::settingsChanged, this, &MainWindow::applySettings);
    connect(&s_, &AppState::statusChanged, this, &MainWindow::onStatus);
    connect(&s_, &AppState::message, this, &MainWindow::onMessage);
    connect(&s_, &AppState::changed, this, [this] {
        setWindowTitle(s_.perf.valid && s_.connected() ? s_.perf.name + " - Cue" : "Cue");
    });

    // While the engine is away, the view says so and shows what it last said.
    auto* waiting = new QTimer(this);
    connect(waiting, &QTimer::timeout, this, [this] {
        if (!s_.connected())
            if (Surface* v = views_.value(current_))
                v->update();
    });
    waiting->start(1000);

    toast_ = new Toast(this, toastText_, toastError_);
    toast_->hide();
    toastTimer_.setSingleShot(true);
    connect(&toastTimer_, &QTimer::timeout, toast_, &QWidget::hide);
    showView(s_.settings.startView);
}

void MainWindow::showView(const QString& view) {
    Surface* v = views_.value(view);
    if (!v)
        return;
    if (view == "tuner" && current_ != "tuner")
        beforeTuner_ = current_.isEmpty() || current_ == "settings" ? "stage" : current_;
    // Back from the plug-ins, the chain view is as it was left.
    if (view == "chains" && (current_ == "plugins" || (current_ == "tuner" && beforeTuner_ == "chains")))
        static_cast<ChainView*>(v)->resume();
    current_ = view;
    stack_->setCurrentWidget(v);
    v->setFocus();
    toast_->raise();
}

void MainWindow::applySettings() {
    const bool full = s_.settings.fullScreen;
    if (full != isFullScreen())
        full ? showFullScreen() : showNormal();
    for (Surface* v : views_)
        v->update();
}

// The inputs muted (the tuner pedal, most often): the tuner comes up, and
// goes back when they are live again.
void MainWindow::onStatus() {
    const bool muted = s_.status.valid && s_.status.muted;
    if (muted && !wasMuted_ && s_.settings.tunerWhenMuted && current_ != "tuner") {
        showView("tuner");
        autoTuner_ = true;
    } else if (!muted && wasMuted_ && autoTuner_) {
        autoTuner_ = false;
        if (current_ == "tuner")
            showView(beforeTuner_);
    }
    wasMuted_ = muted;
}

void MainWindow::onMessage(const QString& text, bool error) {
    toastText_ = text;
    toastError_ = error;
    resizeEvent(nullptr);
    toast_->show();
    toast_->raise();
    toast_->update();
    toastTimer_.start(error ? 6000 : 3000);
}

void MainWindow::resizeEvent(QResizeEvent*) {
    const double u = std::clamp(std::min(width() / 1280.0, height() / 860.0), 0.4, 4.0);
    const int w = std::min(width() - 40, static_cast<int>(900 * u)), h = static_cast<int>(64 * u);
    toast_->setGeometry((width() - w) / 2, height() - h - static_cast<int>(24 * u), w, h);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    const bool ctrl = e->modifiers() & Qt::ControlModifier;
    switch (e->key()) {
    case Qt::Key_F11:
        s_.settings.fullScreen = !isFullScreen();
        s_.settings.save();
        applySettings();
        return;
    case Qt::Key_Escape:
        // On the routing board it first lets go of what is selected.
        if (current_ == "chains" && static_cast<ChainView*>(views_.value("chains"))->deselect())
            return;
        autoTuner_ = false;
        showView(current_ == "plugins" ? "chains" : "stage");
        return;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:
        if (current_ == "chains" && static_cast<ChainView*>(views_.value("chains"))->deleteSelected())
            return;
        break;
    case Qt::Key_Q:
        if (ctrl) {
            qApp->quit();
            return;
        }
        break;
    case Qt::Key_S:
        if (ctrl) {
            s_.save();
            return;
        }
        break;
    case Qt::Key_Z:
        if (ctrl) {
            e->modifiers() & Qt::ShiftModifier ? s_.redo() : s_.undo();
            return;
        }
        break;
    case Qt::Key_Y:
        if (ctrl) {
            s_.redo();
            return;
        }
        break;
    case Qt::Key_T:
        autoTuner_ = false;
        showView(current_ == "tuner" ? beforeTuner_ : "tuner");
        return;
    case Qt::Key_PageDown:
    case Qt::Key_Right:
        if (current_ == "stage" && s_.playing() >= 0) {
            s_.step(+1);
            return;
        }
        break;
    case Qt::Key_PageUp:
    case Qt::Key_Left:
        if (current_ == "stage" && s_.playing() >= 0) {
            s_.step(-1);
            return;
        }
        break;
    }
    QWidget::keyPressEvent(e);
}

} // namespace cue::gui
