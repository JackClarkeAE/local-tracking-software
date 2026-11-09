#include "biofeedback_timeline.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr int kHeaderWidth = 168;
constexpr int kRulerHeight = 24;
const QColor kBg(15, 19, 26);
const QColor kHeaderBg(19, 25, 34);
const QColor kRulerBg(23, 29, 38);
const QColor kGrid(54, 66, 82);
const QColor kText(220, 230, 238);
const QColor kMuted(140, 152, 167);
const QColor kPlayhead(242, 184, 75);

QString formatTime(double t, double step) {
    if (t < 0) t = 0;
    const int minutes = (int)(t / 60.0);
    const double secs = t - minutes * 60.0;
    if (step < 1.0) return QString("%1:%2").arg(minutes).arg(secs, 4, 'f', 1, QChar('0'));
    return QString("%1:%2").arg(minutes).arg((int)std::round(secs), 2, 10, QChar('0'));
}

QString formatValue(float v, const QString& unit) {
    if (!std::isfinite(v)) return "—";
    const int decimals = (unit == "mm") ? 0 : 1;
    return QString::number(v, 'f', decimals);
}

} // namespace

BiofeedbackTimeline::BiofeedbackTimeline(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(120);
    setMouseTracking(false);
    setFocusPolicy(Qt::ClickFocus);  // keys then reach the tab (transport shortcuts)
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void BiofeedbackTimeline::setFrameTimes(std::vector<double> times) {
    times_ = std::move(times);
    fitAll();
}

void BiofeedbackTimeline::setFlags(std::vector<PlaybackFlag> flags) {
    flags_ = std::move(flags);
    invalidate();
}

void BiofeedbackTimeline::setTracks(std::vector<Track> tracks) {
    tracks_ = std::move(tracks);
    invalidate();
}

double BiofeedbackTimeline::duration() const {
    return times_.empty() ? 0.0 : times_.back();
}

void BiofeedbackTimeline::fitAll() {
    viewStart_ = 0.0;
    viewLength_ = std::max(1.0, duration());
    invalidate();
}

void BiofeedbackTimeline::setCurrentTime(double t) {
    current_ = t;
    // Page the view along with the playhead (like an editor's auto-scroll)
    if (!scrubbing_ && (t < viewStart_ || t > viewStart_ + viewLength_)) {
        viewStart_ = t - viewLength_ * 0.1;
        clampView();
    }
    update();
}

QRectF BiofeedbackTimeline::plotRect() const {
    return QRectF(kHeaderWidth, 0, std::max(1, width() - kHeaderWidth - 8), height());
}

double BiofeedbackTimeline::timeToX(double t) const {
    const QRectF r = plotRect();
    return r.left() + (t - viewStart_) / viewLength_ * r.width();
}

double BiofeedbackTimeline::xToTime(double x) const {
    const QRectF r = plotRect();
    return viewStart_ + (x - r.left()) / r.width() * viewLength_;
}

void BiofeedbackTimeline::clampView() {
    const double dur = std::max(0.5, duration());
    viewLength_ = std::clamp(viewLength_, 0.5, std::max(0.5, dur));
    viewStart_ = std::clamp(viewStart_, 0.0, std::max(0.0, dur - viewLength_));
}

int BiofeedbackTimeline::frameIndexAt(double t) const {
    if (times_.empty()) return -1;
    auto it = std::upper_bound(times_.begin(), times_.end(), t);
    if (it == times_.begin()) return 0;
    return (int)std::distance(times_.begin(), it) - 1;
}

void BiofeedbackTimeline::paintEvent(QPaintEvent*) {
    const qreal dpr = devicePixelRatioF();
    const QSize px = (QSizeF(size()) * dpr).toSize();
    if (cacheDirty_ || cache_.size() != px || cache_.devicePixelRatio() != dpr ||
        cacheViewStart_ != viewStart_ || cacheViewLength_ != viewLength_) {
        cache_ = QPixmap(px);
        cache_.setDevicePixelRatio(dpr);
        QPainter cp(&cache_);
        renderStatic(cp);
        cacheDirty_ = false;
        cacheViewStart_ = viewStart_;
        cacheViewLength_ = viewLength_;
    }
    QPainter p(this);
    p.drawPixmap(0, 0, cache_);
    drawLiveValues(p);
    drawPlayhead(p);
}

void BiofeedbackTimeline::renderStatic(QPainter& p) {
    p.fillRect(rect(), kBg);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRectF plot = plotRect();
    const int nTracks = (int)tracks_.size();
    const double trackTop = kRulerHeight;
    const double trackH = nTracks > 0 ? (height() - trackTop) / nTracks : 0.0;

    // Header column + ruler background
    p.fillRect(QRectF(0, 0, kHeaderWidth, height()), kHeaderBg);
    p.fillRect(QRectF(0, 0, width(), kRulerHeight), kRulerBg);

    QFont small = font();
    small.setPointSizeF(7.5);
    QFont label = font();
    label.setPointSizeF(8.0);
    label.setBold(true);
    const QFontMetrics fmSmall(small);

    p.setFont(small);
    p.setPen(kMuted);
    p.drawText(QRectF(10, 0, kHeaderWidth - 12, kRulerHeight), Qt::AlignVCenter | Qt::AlignLeft, "TIME");

    // --- Ruler ticks: pick a step that keeps labels ~80px apart
    if (!times_.empty()) {
        static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
        double step = steps[0];
        for (double s : steps) {
            step = s;
            if (s / viewLength_ * plot.width() >= 80.0) break;
        }
        const double first = std::ceil(viewStart_ / step) * step;
        for (double t = first; t <= viewStart_ + viewLength_ + 1e-9; t += step) {
            const double x = timeToX(t);
            p.setPen(kGrid);
            p.drawLine(QPointF(x, kRulerHeight - 7), QPointF(x, height()));
            p.setPen(kMuted);
            p.drawText(QRectF(x + 3, 0, 70, kRulerHeight - 4), Qt::AlignLeft | Qt::AlignVCenter, formatTime(t, step));
            // minor ticks
            p.setPen(kGrid);
            for (int k = 1; k < 5; ++k) {
                const double xm = timeToX(t + step * k / 5.0);
                if (xm > plot.right()) break;
                p.drawLine(QPointF(xm, kRulerHeight - 4), QPointF(xm, kRulerHeight));
            }
        }
    }

    // --- Tracks
    for (int ti = 0; ti < nTracks; ++ti) {
        const Track& track = tracks_[ti];
        const QRectF row(0, trackTop + ti * trackH, width(), trackH);
        const QRectF area(plot.left(), row.top() + 3, plot.width(), std::max(4.0, row.height() - 6));

        p.setPen(QColor(32, 40, 52));
        p.drawLine(QPointF(0, row.top()), QPointF(width(), row.top()));

        // y range over the visible window, all series
        float lo = std::numeric_limits<float>::max(), hi = -std::numeric_limits<float>::max();
        int i0 = std::max(0, frameIndexAt(viewStart_));
        int i1 = std::min((int)times_.size() - 1, frameIndexAt(viewStart_ + viewLength_) + 1);
        for (const Series& s : track.series) {
            for (int i = i0; i <= i1 && i < (int)s.values.size(); ++i) {
                const float v = s.values[i];
                if (!std::isfinite(v)) continue;
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        const bool hasData = lo <= hi;
        if (hasData) {
            if (hi - lo < 1e-3f) { lo -= 1.0f; hi += 1.0f; }
            const float pad = (hi - lo) * 0.12f;
            lo -= pad;
            hi += pad;
        }
        auto yOf = [&](float v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };

        if (hasData && lo < 0 && hi > 0) {
            p.setPen(QPen(QColor(70, 84, 102), 1, Qt::DotLine));
            p.drawLine(QPointF(area.left(), yOf(0)), QPointF(area.right(), yOf(0)));
        }

        // Curves: min/max per pixel column so long sessions stay fast and honest.
        // Cosmetic pens use Qt's fast line rasteriser; stroking one big
        // antialiased path per series took seconds on a 20-minute session.
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setClipRect(area.adjusted(0, -2, 0, 2));
        if (hasData && !times_.empty()) {
            const int cols = (int)area.width();
            for (const Series& s : track.series) {
                QPen pen(s.color, 1.0);
                pen.setCosmetic(true);
                if (s.dashed) pen.setStyle(Qt::DashLine);
                p.setPen(pen);
                std::vector<QPolygonF> runs(1);
                std::vector<QLineF> spans;  // min..max within a column
                int idx = i0;
                for (int c = 0; c <= cols; ++c) {
                    const double tEnd = xToTime(area.left() + c + 1);
                    float mn = std::numeric_limits<float>::max(), mx = -mn, last = NAN;
                    int n = 0;
                    bool missing = false;
                    while (idx < (int)times_.size() && idx < (int)s.values.size() && times_[idx] < tEnd) {
                        const float v = s.values[idx++];
                        if (!std::isfinite(v)) { missing = true; continue; }
                        mn = std::min(mn, v);
                        mx = std::max(mx, v);
                        last = v;
                        ++n;
                    }
                    if (n == 0) {
                        if (missing && !runs.back().isEmpty()) runs.emplace_back();  // untracked: break the line
                        if (idx >= (int)times_.size() || idx >= (int)s.values.size()) break;
                        continue;  // empty column is normal when zoomed in
                    }
                    const double x = area.left() + c;
                    if (!s.dashed && yOf(mn) - yOf(mx) > 1.5) spans.emplace_back(x, yOf(mn), x, yOf(mx));
                    runs.back() << QPointF(x, yOf(last));
                }
                for (const QPolygonF& run : runs)
                    if (run.size() > 1) p.drawPolyline(run);
                if (!spans.empty()) p.drawLines(spans.data(), (int)spans.size());
            }
        }
        p.setClipping(false);
        p.setRenderHint(QPainter::Antialiasing, false);

        // Header: track name, unit, legend with values at the playhead
        p.setFont(label);
        p.setPen(kText);
        p.drawText(QRectF(10, row.top() + 2, kHeaderWidth - 14, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   track.unit.isEmpty() ? track.name : QString("%1 (%2)").arg(track.name, track.unit));
        if (hasData && area.height() > 24) {
            p.setPen(QColor(100, 112, 128));
            p.drawText(QRectF(area.left() + 4, area.top(), 80, 12), Qt::AlignLeft | Qt::AlignTop,
                       QString::number(hi, 'f', track.unit == "mm" ? 0 : 1));
            p.drawText(QRectF(area.left() + 4, area.bottom() - 12, 80, 12), Qt::AlignLeft | Qt::AlignBottom,
                       QString::number(lo, 'f', track.unit == "mm" ? 0 : 1));
        }
    }

    // --- Flags (ruler markers + faint guide lines)
    static const QColor flagColors[] = {QColor(255, 150, 150), QColor(150, 200, 255), QColor(150, 255, 150),
                                        QColor(255, 220, 130), QColor(200, 150, 255), QColor(255, 180, 220)};
    p.setFont(small);
    for (int i = 0; i < (int)flags_.size(); ++i) {
        const double x = timeToX(flags_[i].timeSeconds);
        if (x < plot.left() || x > plot.right()) continue;
        const QColor c = flagColors[i % 6];
        p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 70), 1));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, height()));
        const QString text = QString::fromStdString(flags_[i].label);
        QRectF tr(x + 2, 2, fmSmall.horizontalAdvance(text) + 8, 13);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(10, 13, 18, 220));
        p.drawRect(tr);
        p.setPen(c);
        p.drawText(tr, Qt::AlignCenter, text);
        p.drawLine(QPointF(x, 0), QPointF(x, kRulerHeight));
    }

    // Separator between header and plot
    p.setPen(QColor(44, 54, 68));
    p.drawLine(QPointF(kHeaderWidth - 0.5, 0), QPointF(kHeaderWidth - 0.5, height()));

    if (times_.empty()) {
        p.setFont(label);
        p.setPen(kMuted);
        p.drawText(plot, Qt::AlignCenter, "Open a session to see its timeline");
    }
}

void BiofeedbackTimeline::drawLiveValues(QPainter& p) {
    // Values at the playhead, one compact "name value" chunk per series
    const int nTracks = (int)tracks_.size();
    if (nTracks == 0 || times_.empty()) return;
    const double trackH = (height() - kRulerHeight) / (double)nTracks;
    const int frame = frameIndexAt(current_);
    QFont small = font();
    small.setPointSizeF(7.5);
    p.setFont(small);
    const QFontMetrics fmSmall(small);
    for (int ti = 0; ti < nTracks; ++ti) {
        const Track& track = tracks_[ti];
        const double rowTop = kRulerHeight + ti * trackH;
        const double ly = rowTop + 18;
        if (ly + fmSmall.height() > rowTop + trackH + 2) continue;
        double lx = 12;
        for (const Series& s : track.series) {
            const float v = (frame >= 0 && frame < (int)s.values.size()) ? s.values[frame] : NAN;
            const QString name = s.name + " ";
            const QString value = formatValue(v, track.unit);
            const double need = fmSmall.horizontalAdvance(name) + fmSmall.horizontalAdvance(value) + 8;
            if (lx + need > kHeaderWidth - 4) break;
            QColor dim = s.color;
            dim.setAlpha(150);
            p.setPen(dim);
            p.drawText(QPointF(lx, ly + fmSmall.ascent()), name);
            lx += fmSmall.horizontalAdvance(name);
            p.setPen(s.color);
            p.drawText(QPointF(lx, ly + fmSmall.ascent()), value);
            lx += fmSmall.horizontalAdvance(value) + 8;
        }
    }
}

void BiofeedbackTimeline::drawPlayhead(QPainter& p) {
    const QRectF plot = plotRect();
    if (!times_.empty()) {
        const double x = timeToX(current_);
        if (x >= plot.left() - 1 && x <= plot.right() + 1) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(kPlayhead, 1.5));
            p.drawLine(QPointF(x, kRulerHeight - 2), QPointF(x, height()));
            QPainterPath head;
            head.moveTo(x - 6, 3);
            head.lineTo(x + 6, 3);
            head.lineTo(x + 6, kRulerHeight - 9);
            head.lineTo(x, kRulerHeight - 2);
            head.lineTo(x - 6, kRulerHeight - 9);
            head.closeSubpath();
            p.setPen(Qt::NoPen);
            p.setBrush(kPlayhead);
            p.drawPath(head);
        }
    }

}

void BiofeedbackTimeline::seekFromX(double x) {
    if (times_.empty()) return;
    const double t = std::clamp(xToTime(x), 0.0, duration());
    current_ = t;
    update();
    emit seekRequested(t);
}

void BiofeedbackTimeline::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || e->position().x() < kHeaderWidth || times_.empty()) return;
    scrubbing_ = true;
    emit scrubStarted();
    seekFromX(e->position().x());
}

void BiofeedbackTimeline::mouseMoveEvent(QMouseEvent* e) {
    if (scrubbing_) seekFromX(e->position().x());
}

void BiofeedbackTimeline::mouseReleaseEvent(QMouseEvent*) {
    scrubbing_ = false;
}

void BiofeedbackTimeline::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->position().y() <= kRulerHeight) fitAll();
}

void BiofeedbackTimeline::wheelEvent(QWheelEvent* e) {
    if (times_.empty()) return;
    const double steps = e->angleDelta().y() / 120.0;
    if (e->modifiers() & Qt::ShiftModifier) {
        viewStart_ -= steps * viewLength_ * 0.1;
    } else {
        // Zoom around the cursor
        const double anchor = xToTime(e->position().x());
        const double factor = std::pow(0.85, steps);
        const double newLen = std::clamp(viewLength_ * factor, 0.5, std::max(0.5, duration()));
        const double frac = (anchor - viewStart_) / viewLength_;
        viewLength_ = newLen;
        viewStart_ = anchor - frac * newLen;
    }
    clampView();
    update();
    e->accept();
}
