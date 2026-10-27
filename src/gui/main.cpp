// cue-app: the Cue desktop app. It shows and plays what a running `cue run`
// is doing, over its control socket; the sound never depends on it, so it can
// be closed and opened again mid-song.

#include "gui/link.h"
#include "gui/mainwindow.h"
#include "gui/state.h"
#include "gui/theme.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace cue::gui;

namespace {

void usage() {
    std::printf("cue-app %s: the Cue app\n\n"
                "  cue-app [options]\n\n"
                "  --start FILE.cue     start the engine (cue run FILE.cue) if none is running; it keeps\n"
                "                       playing when the app closes\n"
                "  --audio none         ...without a sound card, to look at or change a performance anywhere\n"
                "  --view stage|tuner|chains   the view to open with (else the setting)\n"
                "  --fullscreen, --windowed\n"
                "  --scale N            size of everything, 0.5 to 2.5\n"
                "  --socket PATH        the engine's control socket (as cue run --socket)\n"
                "  --size WxH           the window's size\n"
                "  --screenshot FILE    save a picture of the window once the engine has answered, then quit\n"
                "  --version\n",
                CUE_VERSION);
}

// Whether an engine answers at the socket now.
bool engineAnswers(const QString& path) {
    QLocalSocket probe;
    probe.connectToServer(path);
    const bool up = probe.waitForConnected(300);
    probe.abort();
    return up;
}

// cue run, detached in a session of its own so closing the app (or its
// terminal) leaves it playing. Its output goes to ~/.local/state/cue/engine.log.
bool startEngine(const QString& file, const QString& socket, bool noAudio, AppState& stateOf, QString* error) {
    QString cue = QCoreApplication::applicationDirPath() + "/cue";
    if (!QFileInfo(cue).isExecutable())
        cue = QStandardPaths::findExecutable("cue");
    if (cue.isEmpty()) {
        *error = "cannot find the cue program beside cue-app or on the PATH";
        return false;
    }
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty())
        state = QDir::homePath() + "/.local/state";
    QDir().mkpath(state + "/cue");
    QStringList args = {"run", QFileInfo(file).absoluteFilePath()};
    if (!socket.isEmpty())
        args << "--socket" << socket;
    if (noAudio)
        args << "--audio" << "none";
    if (!stateOf.settings.takeCard)
        args << "--leave-device";
    QString program = cue;
    const QString setsid = QStandardPaths::findExecutable("setsid");
    if (!setsid.isEmpty()) {
        args.prepend(cue);
        program = setsid;
    }
    const QString log = state + "/cue/engine.log";
    stateOf.engineLog = log;
    stateOf.engineLogFrom = QFileInfo(log).size();
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.setStandardInputFile(QProcess::nullDevice());
    p.setStandardOutputFile(log, QIODevice::Append);
    p.setStandardErrorFile(log, QIODevice::Append);
    if (!p.startDetached()) {
        *error = "could not start " + cue;
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // Before Qt starts, so these work without a display (over ssh, say).
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--version") {
            std::printf("cue-app %s (Qt %s)\n", CUE_VERSION, qVersion());
            return 0;
        }
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        }
    }
    if (!std::getenv("DISPLAY") && !std::getenv("WAYLAND_DISPLAY") && !std::getenv("QT_QPA_PLATFORM")) {
        std::fprintf(stderr, "cue-app: there is no screen to show it on (DISPLAY is not set). Start it on the\n"
                             "computer's desktop, or from its autostart (see docs/app.md).\n");
        return 1;
    }

    QApplication app(argc, argv);
    app.setApplicationName("Cue");
    app.setOrganizationName("TwoStorms");
    app.setApplicationVersion(CUE_VERSION);

    QString socket, view, start, screenshot;
    QSize size(1280, 800);
    bool full = false, windowed = false, noAudio = false;
    double scale = 0;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args[i];
        auto next = [&]() -> QString { return i + 1 < args.size() ? args[++i] : QString(); };
        if (a == "--socket")
            socket = next();
        else if (a == "--view")
            view = next();
        else if (a == "--start")
            start = next();
        else if (a == "--scale")
            scale = next().toDouble();
        else if (a == "--audio")
            noAudio = next() == "none";
        else if (a == "--screenshot")
            screenshot = next();
        else if (a == "--size") {
            const QStringList wh = next().split('x');
            if (wh.size() == 2)
                size = QSize(wh[0].toInt(), wh[1].toInt());
        } else if (a == "--fullscreen")
            full = true;
        else if (a == "--windowed")
            windowed = true;
        else {
            std::fprintf(stderr, "cue-app: unknown option %s\n\n", qPrintable(a));
            usage();
            return 2;
        }
    }

    loadFonts();
    app.setFont(typeface(Face::Text, 14));

    EngineLink link(socket);
    AppState state(link);
    state.settings.load();
    if (view == "stage" || view == "tuner" || view == "chains")
        state.settings.startView = view;
    if (scale > 0)
        state.settings.scale = std::clamp(scale, 0.5, 2.5);
    if (full || windowed)
        state.settings.fullScreen = full;

    if (!start.isEmpty() && !engineAnswers(link.socketPath())) {
        QString err;
        if (startEngine(start, socket, noAudio, state, &err))
            std::printf("cue-app: started cue run %s\n", qPrintable(start));
        else
            std::fprintf(stderr, "cue-app: %s\n", qPrintable(err));
    }

    MainWindow window(state);
    window.resize(size);
    if (!screenshot.isEmpty()) {
        // A second after the first status (or after five without one), so the meters and tuner have moved.
        auto shoot = [&window, screenshot] {
            window.grab().save(screenshot);
            QCoreApplication::quit();
        };
        QObject::connect(&state, &AppState::statusChanged, &window, [shoot, done = false]() mutable {
            if (!done) {
                done = true;
                QTimer::singleShot(1200, shoot);
            }
        });
        QTimer::singleShot(5000, shoot);
    }
    if (state.settings.fullScreen)
        window.showFullScreen();
    else
        window.show();
    link.start();
    return app.exec();
}
