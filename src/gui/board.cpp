#include "gui/board.h"

#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>

namespace cue::gui {

Shape shapeOf(const PluginInfo& p) {
    const QString id = p.id.toLower(), label = p.label.toLower();
    const QString file = p.path.section('/', -1).toLower();
    if (id == "com.twostorms.gainstage" || label.contains("gain stage") || file.contains("gain stage"))
        return Shape::GainStage;
    static const QRegularExpression act("\\bact\\b");
    if (id == "com.twostorms.act" || act.match(label).hasMatch() || (id.isEmpty() && act.match(file).hasMatch()))
        return Shape::Act;
    return Shape::Hexagon;
}

namespace {

bool isBlock(const PortInfo& p, int n) { return p.kind == PortInfo::Kind::Block && p.index >= 0 && p.index < n; }

double halfWidthOf(Shape s) { return s == Shape::GainStage ? Board::gainStageR : s == Shape::Act ? Board::actR : Board::blockW / 2; }

} // namespace

Board::Board(const ChainInfo& c, int numOutputs, const QHash<int, QPointF>& moved) {
    const int n = c.blocks.size();
    for (const PluginInfo& p : c.blocks)
        shapes_ << shapeOf(p);

    // The order the engine plays them in: what feeds a plug-in first, the
    // lowest slot first.
    QVector<int> waiting(n, 0), order;
    for (const WireInfo& w : c.wires)
        if (isBlock(w.from, n) && isBlock(w.to, n))
            ++waiting[w.to.index];
    QVector<bool> done(n, false);
    for (int k = 0; k < n; ++k) {
        int pick = -1;
        for (int b = 0; b < n && pick < 0; ++b)
            if (!done[b] && waiting[b] == 0)
                pick = b;
        for (int b = 0; b < n && pick < 0; ++b) // a loop, which the engine refuses: the rest in order
            if (!done[b])
                pick = b;
        done[pick] = true;
        order << pick;
        for (const WireInfo& w : c.wires)
            if (isBlock(w.from, n) && w.from.index == pick && isBlock(w.to, n))
                --waiting[w.to.index];
    }

    // Rows: a plug-in goes on after what feeds it when that is the last of
    // its row (the input starts row 0) and does not also feed an output,
    // where a row ends; otherwise it starts a row of its own. So a chain
    // that splits after a plug-in has a row for each way.
    QVector<bool> feedsOutput(n, false);
    for (const WireInfo& w : c.wires)
        if (isBlock(w.from, n) && w.to.kind == PortInfo::Kind::Output)
            feedsOutput[w.from.index] = true;
    struct Row {
        int source = -2; // the slot that feeds it, -1 the input, -2 nothing
        QVector<int> blocks;
    };
    QVector<Row> rows(1);
    rows[0].source = -1;
    QVector<int> rowOf(n, -1), colOf(n, 1);
    for (int b : order) {
        int row = -1, source = -2;
        for (const WireInfo& w : c.wires) {
            if (!isBlock(w.to, n) || w.to.index != b || w.to.key)
                continue;
            const int from = w.from.kind == PortInfo::Kind::Input ? -1 : isBlock(w.from, n) ? w.from.index : -2;
            if (from == -2 || (from >= 0 && rowOf[from] < 0))
                continue;
            if (source == -2)
                source = from;
            const int r = from < 0 ? 0 : rowOf[from];
            const QVector<int>& bl = rows[r].blocks;
            if (from < 0 ? bl.isEmpty() : bl.last() == from && !feedsOutput[from]) {
                row = r;
                break;
            }
        }
        if (row < 0 && rows[0].blocks.isEmpty() && source == -2)
            row = 0;
        if (row < 0) {
            rows.append(Row{source, {}});
            row = rows.size() - 1;
        }
        rows[row].blocks << b;
        rowOf[b] = row;
    }
    // Columns: a row starts after what feeds it, or further left if that
    // would make the board wider (its wire then comes back to it).
    int widest = 0;
    for (int r = 0; r < rows.size(); ++r) {
        const int len = rows[r].blocks.size();
        int start = 1;
        if (r > 0 && rows[r].source >= 0) {
            start = colOf[rows[r].source] + 1;
            if (start + len - 1 > widest)
                start = std::max(1, widest - len + 1);
        }
        for (int i = 0; i < len; ++i)
            colOf[rows[r].blocks[i]] = start + i;
        widest = std::max(widest, start + len - 1);
    }
    auto center = [](int col, int row) { return QPointF(edge + (col + 0.5) * colPitch, edge + (row + 0.5) * rowPitch); };
    laidOut.resize(n);
    block.resize(n);
    for (int b = 0; b < n; ++b) {
        laidOut[b] = center(colOf[b], rowOf[b]);
        block[b] = moved.contains(b) ? moved.value(b) : c.blocks[b].placed ? c.blocks[b].at : laidOut[b];
    }

    // The input left of every block, beside the first it feeds; the outputs
    // right of them, each beside what feeds it, none on top of another.
    double minX = center(1, 0).x(), maxX = minX;
    if (n > 0) {
        minX = std::numeric_limits<double>::max();
        maxX = std::numeric_limits<double>::lowest();
        for (int b = 0; b < n; ++b) {
            const double hw = halfWidthOf(shapes_[b]);
            minX = std::min(minX, block[b].x() - hw + blockW / 2);
            maxX = std::max(maxX, block[b].x() + hw - blockW / 2);
        }
    }
    double inputY = center(0, 0).y();
    for (int b : order) {
        const bool fed = std::any_of(c.wires.begin(), c.wires.end(), [&](const WireInfo& w) {
            return w.from.kind == PortInfo::Kind::Input && isBlock(w.to, n) && w.to.index == b && !w.to.key;
        });
        if (fed) {
            inputY = block[b].y();
            break;
        }
    }
    input = QPointF(minX - colPitch, inputY);
    output.resize(numOutputs);
    QVector<QPair<double, int>> want;
    for (int o = 0; o < numOutputs; ++o) {
        double y = std::numeric_limits<double>::max();
        for (const WireInfo& w : c.wires)
            if (w.to.kind == PortInfo::Kind::Output && w.to.index == o) {
                y = w.from.kind == PortInfo::Kind::Input ? input.y() : isBlock(w.from, n) ? block[w.from.index].y() : y;
                break;
            }
        want << qMakePair(y, o);
    }
    std::stable_sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    double last = std::numeric_limits<double>::lowest();
    const double gap = ioH + 34;
    for (const auto& [y, o] : want) {
        const double at = y == std::numeric_limits<double>::max() ? (last == std::numeric_limits<double>::lowest() ? input.y() : last + gap)
                                                                   : std::max(y, last + gap);
        output[o] = QPointF(maxX + colPitch, at);
        last = at;
    }

    QRectF all = ioBox(input);
    for (int b = 0; b < n; ++b)
        all |= box(b);
    for (const QPointF& o : output)
        all |= ioBox(o);
    bounds = all.adjusted(-edge, -edge, edge, edge);
}

QRectF Board::box(int slot) const {
    const QPointF c = block.value(slot);
    const Shape s = shapes_.value(slot, Shape::Hexagon);
    if (s == Shape::Hexagon)
        return {c.x() - blockW / 2, c.y() - blockH / 2, blockW, blockH};
    const double r = s == Shape::GainStage ? gainStageR : actR;
    return {c.x() - r, c.y() - r, 2 * r, 2 * r};
}

QPointF Board::outPoint(const PortInfo& p) const {
    switch (p.kind) {
    case PortInfo::Kind::Input: return input + QPointF(ioW / 2, 0);
    case PortInfo::Kind::Block: return block.value(p.index) + QPointF(halfWidthOf(shapes_.value(p.index)), 0);
    case PortInfo::Kind::Output: break;
    }
    return output.value(p.index);
}

QPointF Board::inPoint(const PortInfo& p) const {
    switch (p.kind) {
    case PortInfo::Kind::Input: return input;
    case PortInfo::Kind::Block: break;
    case PortInfo::Kind::Output: return output.value(p.index) - QPointF(ioW / 2, 0);
    }
    const QPointF c = block.value(p.index);
    const Shape s = shapes_.value(p.index);
    if (!p.key)
        return c - QPointF(halfWidthOf(s), 0);
    // The key: low on the left edge, at the tip of its triangle.
    if (s == Shape::Hexagon)
        return c + QPointF(-blockW / 2 + 6, blockH * 0.375);
    const double r = halfWidthOf(s);
    return c + QPointF(-r * 0.707 - 4, r * 0.707);
}

QPainterPath Board::path(const WireInfo& w) const {
    if (w.jump) {
        const QPointF a = outPoint(w.from), b = inPoint(w.to), o = jumpOutAt(w), i = jumpInAt(w);
        // Out to the right and in from the left, whichever way the letters are.
        QPainterPath p(a);
        p.cubicTo(a + QPointF(16, 0), o, o);
        p.moveTo(i);
        p.cubicTo(i, b - QPointF(16, 0), b);
        return p;
    }
    return path(outPoint(w.from), inPoint(w.to), w.from.kind == PortInfo::Kind::Block ? w.from.index : -1,
                w.to.kind == PortInfo::Kind::Block ? w.to.index : -1);
}

// Straight along a row; an S-curve to a row below or above; by the lane
// between rows when blocks are in the way, or when it has to go back left.
QPainterPath Board::path(QPointF a, QPointF b, int fromSlot, int toSlot) const {
    QPainterPath p(a);
    const double rr = 18;
    const bool back = b.x() < a.x() + 2;
    bool blocked = false;
    if (!back && std::fabs(a.y() - b.y()) < 0.5) {
        const QRectF seg(a.x(), a.y() - 3, b.x() - a.x(), 6);
        for (int s = 0; s < block.size() && !blocked; ++s)
            blocked = s != fromSlot && s != toSlot && box(s).intersects(seg);
    }
    if (!back && !blocked) {
        if (std::fabs(a.y() - b.y()) < 0.5) {
            p.lineTo(b);
        } else {
            const double dx = std::max(30.0, (b.x() - a.x()) / 2);
            p.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        }
        return p;
    }
    const double lane = a.y() + (b.y() >= a.y() ? 1 : -1) * rowPitch / 2;
    if (back) {
        p.cubicTo(QPointF(a.x() + 2 * rr, a.y()), QPointF(a.x() + 2 * rr, lane), QPointF(a.x() + rr, lane));
        p.lineTo(b.x() - rr, lane);
        p.cubicTo(QPointF(b.x() - 2 * rr, lane), QPointF(b.x() - 2 * rr, b.y()), b);
    } else {
        p.cubicTo(QPointF(a.x() + rr, a.y()), QPointF(a.x() + rr, lane), QPointF(a.x() + 2 * rr, lane));
        p.lineTo(b.x() - 2 * rr, lane);
        p.cubicTo(QPointF(b.x() - rr, lane), QPointF(b.x() - rr, b.y()), b);
    }
    return p;
}

QPointF Board::freeNear(QPointF want) const {
    auto clear = [&](QPointF at) {
        const QRectF r(at.x() - blockW / 2 - 12, at.y() - blockH / 2 - 12, blockW + 24, blockH + 24);
        for (int b = 0; b < block.size(); ++b)
            if (box(b).intersects(r))
                return false;
        if (ioBox(input).intersects(r))
            return false;
        for (const QPointF& o : output)
            if (ioBox(o).intersects(r))
                return false;
        return true;
    };
    for (int ring = 0; ring < 6; ++ring)
        for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != ring)
                    continue;
                const QPointF at = want + QPointF(dx * colPitch, dy * rowPitch / 2);
                if (clear(at))
                    return at;
            }
    return want + QPointF(0, rowPitch);
}

} // namespace cue::gui
