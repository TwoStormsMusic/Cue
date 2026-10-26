#pragma once

// The chain view: a chain's plug-ins as blocks and wires, as the routing
// mockup draws them, laid out from the performance. Tapping a block shows its
// parameters as sliders and switches it on or off; Save keeps it all in the
// .cue file. Moving and wiring blocks comes with the routing editor.

#include "gui/surface.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QTimer>

namespace cue::gui {

class ChainView : public Surface {
    Q_OBJECT
public:
    explicit ChainView(AppState& state, QWidget* parent = nullptr);

    // The chain shown (the one playing when the view opens) and the
    // plug-in selected in it, -1 for none.
    int shownChain() const { return chain_; }
    int selectedSlot() const { return slot_; }
    void showChain(int chain);
    void selectSlot(int slot);

protected:
    void paint(QPainter& p) override;
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    void sidebar(QPainter& p, const QRectF& r);
    void canvas(QPainter& p, const QRectF& r, const ChainInfo& c);
    void legend(QPainter& p, const QRectF& r);
    void panel(QPainter& p, const QRectF& r, const ChainInfo& c);
    void requestPlugin();
    void setParam(const QJsonObject& param, double value, bool done);

    int chain_ = -1, slot_ = -1;
    bool followPlaying_ = true;
    QJsonObject plugin_; // the selected plug-in as the engine last described it
    QTimer refresh_;
    double scroll_ = 0, scrollMax_ = 0;
    QRectF listRect_;
    // A slider being dragged shows where the finger is, not the engine.
    int draggingParam_ = -1;
    double draggingValue_ = 0;
    QElapsedTimer clock_;
    qint64 lastSend_ = 0;
    double scrollFrom_ = 0, scrollStartY_ = -1;
};

} // namespace cue::gui
