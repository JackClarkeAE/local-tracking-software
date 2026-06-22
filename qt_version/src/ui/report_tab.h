#pragma once
#include <QWidget>
#include <QString>
#include <memory>
#include <string>
#include <vector>
#include <utility>

class AppController;
class PlaybackManager;
class QPdfView;
class QPdfDocument;
class QComboBox;
class QPushButton;
class QLabel;
class QVBoxLayout;

// EXPERIMENTAL — Report tab.
// Builds a PDF of biomechanical metrics/graphs from a recorded session. The
// user adds metric blocks via a "+ Add Metric" dialog; each block specifies an
// angle, which statistics, a timeframe (full session / absolute times / a
// start+end flag from the recording), and an aggregation (whole window, or
// per-gait-cycle then averaged). Left side shows a live PDF preview.
//
// Ankle dorsiflexion is intentionally excluded — markerless depth cameras
// cannot measure it reliably (see the note in the Experimental tab).
class ReportTab : public QWidget {
    Q_OBJECT
public:
    explicit ReportTab(AppController* ctrl, QWidget* parent = nullptr);
    ~ReportTab() override;

    void refreshSessions();

    enum TimeMode { TIME_FULL = 0, TIME_ABSOLUTE, TIME_FLAGS };
    enum Agg { AGG_WHOLE = 0, AGG_PER_CYCLE };
    enum Kind { KIND_TABLE = 0, KIND_RADAR };

    // One configured report block (a metric table, or a radar comparison).
    struct Metric {
        int kind = KIND_TABLE;
        int angle = 0;                       // BiomechAngle index (knee/hip only)
        bool rom = true, max = false, mean = false, min = false, graph = false;
        int radarStat = 1;                   // KIND_RADAR: 0=ROM, 1=peak flexion
        int timeMode = TIME_FULL;
        double tStart = 0, tEnd = 0;          // TIME_ABSOLUTE (seconds)
        double flagStart = -1, flagEnd = -1;  // TIME_FLAGS (resolved times)
        QString flagStartLabel, flagEndLabel;
        int agg = AGG_WHOLE;
    };

private slots:
    void onSessionChanged();
    void onAddMetric();
    void onAddRadar();
    void onSaveStandard();
    void onLoadStandard();
    void onExport();

private:
    struct Stats {
        bool valid = false;
        int samples = 0, cycles = 0;
        double minV = 0, maxV = 0, mean = 0, rom = 0;
        std::vector<std::pair<double, double>> windowSeries; // (t, deg) within window
    };

    void loadSession(const std::string& csvPath);
    std::vector<std::pair<double, double>> fullSeries(int angle) const; // (t, deg)
    void windowBounds(const Metric& m, double& t0, double& t1) const;
    Stats computeStats(const Metric& m) const;
    void resolveFlagTimes(Metric& m) const;   // re-resolve flag labels for this session
    void rebuildCards();
    void regenerate();

    AppController* ctrl_ = nullptr;
    QPdfView* pdfView_ = nullptr;
    QPdfDocument* pdfDoc_ = nullptr;
    QComboBox* sessionCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* addBtn_ = nullptr;
    QPushButton* addRadarBtn_ = nullptr;
    QPushButton* exportBtn_ = nullptr;
    QVBoxLayout* cardsLayout_ = nullptr;

    std::vector<Metric> metrics_;
    std::unique_ptr<PlaybackManager> playback_;
    std::string loadedCsv_;
    std::string tempPdfPath_;
};
