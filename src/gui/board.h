#pragma once

// Where the routing board draws a chain: its plug-ins as blocks, the input
// at the left, the outputs at the right, and the path of every wire. In
// board units, before the canvas fits and zooms them. A plug-in put
// somewhere stays there; the others are laid out from the wires, a line of
// plug-ins in series to a row, a branch on a row below.

#include "gui/model.h"

#include <QHash>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QVector>

namespace cue::gui {

enum class Shape { Hexagon, GainStage, Act };
Shape shapeOf(const PluginInfo& p);

struct Board {
    static constexpr double colPitch = 128, rowPitch = 150, edge = 34, blockW = 100, blockH = 74, ioW = 72, ioH = 56;
    static constexpr double gainStageR = 34, actR = 15;

    // moved: blocks being dragged, where they are now.
    Board(const ChainInfo& c, int numOutputs, const QHash<int, QPointF>& moved = {});

    QVector<QPointF> block;  // the centre of each block, by slot
    QVector<QPointF> laidOut; // where each would go if it had not been put anywhere
    QPointF input;            // the centre of the input box
    QVector<QPointF> output;  // ...of each output box, by output number
    QRectF bounds;            // all of it, with an edge around

    // A block's outline and the box around it.
    QRectF box(int slot) const;
    QRectF ioBox(QPointF center) const { return {center.x() - ioW / 2, center.y() - ioH / 2, ioW, ioH}; }
    // Where a wire leaves a port (the right side of what it comes out of)
    // and where one arrives (the left side, or a block's key: the small
    // triangle low on its left).
    QPointF outPoint(const PortInfo& p) const;
    QPointF inPoint(const PortInfo& p) const;
    QPainterPath path(const WireInfo& w) const;
    QPainterPath path(QPointF from, QPointF to, int fromSlot = -1, int toSlot = -1) const;
    // The free place nearest to want for a new block.
    QPointF freeNear(QPointF want) const;

private:
    QVector<Shape> shapes_;
};

} // namespace cue::gui
