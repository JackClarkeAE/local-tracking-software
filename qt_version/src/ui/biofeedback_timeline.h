#pragma once
// EXPERIMENTAL — video-editor style timeline for the Biofeedback Development
// tab: a time ruler with session flags, a draggable playhead, and stacked
// curve tracks (e.g. raw vs target vs output knee angle across the session).

#include "../recording/playback.h"

#include <QColor>
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <vector>

class QPainter;

class BiofeedbackTimeline : public QWidget {
    Q_OBJECT
public:
    struct Series {
        QString name;        // short, shown in the header next to the value
        QColor color;
        bool dashed = false;
        std::vector<float> values;  // one per frame time; NaN = no data
    };
    struct Track {
        QString name;
        QString unit;
        std::vector<Series> series;
    };

    explicit BiofeedbackTimeline(QWidget* parent = nullptr);

    void setFrameTimes(std::vector<double> times);
    void setFlags(std::vector<PlaybackFlag> flags);
    void setTracks(std::vector<Track> tracks);
    void setCurrentTime(double t);
    void fitAll();

    double currentTime() const { return current_; }

signals:
    void seekRequested(double seconds);
    void scrubStarted();

protected:
    void paintEvent(QPaintEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    double duration() const;
    QRectF plotRect() const;  // ruler + tracks area right of the header column
    double xToTime(double x) const;
    double timeToX(double t) const;
    void clampView();
    void seekFromX(double x);
    int frameIndexAt(double t) const;
    void renderStatic(QPainter& p);   // everything except the playhead and live values
    void drawLiveValues(QPainter& p);
    void drawPlayhead(QPainter& p);
    void invalidate() { cacheDirty_ = true; update(); }

    std::vector<double> times_;
    std::vector<PlaybackFlag> flags_;
    std::vector<Track> tracks_;
    double current_ = 0.0;
    double viewStart_ = 0.0;
    double viewLength_ = 10.0;
    bool scrubbing_ = false;

    // The curves are rendered once into a pixmap and reused while only the
    // playhead moves; redrawing tens of thousands of samples (dashed lines in
    // particular) on every frame made long sessions stutter.
    QPixmap cache_;
    bool cacheDirty_ = true;
    double cacheViewStart_ = -1.0, cacheViewLength_ = -1.0;
};
