// cue-convert: a small window that turns a REAPER project into a .cue
// performance and a .cue performance into a REAPER project.
//
//   cue-convert [file]                      open the window (and convert file)
//   cue-convert [file] --screenshot out.png  convert, save a picture of the window, quit

#include "window.h"

#include <QApplication>
#include <QTimer>

#include <cstring>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Cue Converter");
    QString file, shot;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc)
            shot = argv[++i];
        else
            file = QString::fromLocal8Bit(argv[i]);
    }
    cue::converter::Window window;
    window.show();
    if (!shot.isEmpty()) {
        auto save = [&] {
            window.grab().save(shot);
            app.quit();
        };
        if (file.isEmpty())
            QTimer::singleShot(200, save);
        else
            QObject::connect(&window, &cue::converter::Window::finished, &window, [&](bool) { QTimer::singleShot(100, save); });
    }
    if (!file.isEmpty())
        window.convert(file);
    return app.exec();
}
