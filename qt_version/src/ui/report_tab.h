#pragma once
#include <QWidget>
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
class QCheckBox;
class QLabel;

// EXPERIMENTAL — Report tab.
// Takes a recorded session and produces a PDF of biomechanical metrics and
// graphs. Left side: embedded PDF preview (scrollable). Right side: a session
// picker plus per-joint metric checkboxes (ROM / Max / Mean / Min / Graph)
// that regenerate the report live. The report can then be exported anywhere.
class ReportTab : public QWidget {
    Q_OBJECT
public:
    explicit ReportTab(AppController* ctrl, QWidget* parent = nullptr);
    ~ReportTab() override;

    // Repopulate the session dropdown from the recordings directory.
    void refreshSessions();

    enum Stat { STAT_ROM = 0, STAT_MAX, STAT_MEAN, STAT_MIN, STAT_GRAPH, STAT_COUNT };

private slots:
    void onSessionChanged();
    void onSelectionChanged();
    void onExport();

private:
    // Per-angle time series + summary statistics over the loaded session.
    struct AngleStats {
        bool valid = false;
        int samples = 0;
        double minV = 0, maxV = 0, mean = 0, rom = 0;
        std::vector<std::pair<double, double>> series; // (timeSeconds, degrees)
    };

    void loadSession(const std::string& csvPath);
    AngleStats computeAngle(int angleId) const;
    void regenerate();          // build the PDF and reload the preview
    bool anySelection() const;

    AppController* ctrl_ = nullptr;

    QPdfView* pdfView_ = nullptr;
    QPdfDocument* pdfDoc_ = nullptr;
    QComboBox* sessionCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QPushButton* exportBtn_ = nullptr;
    QCheckBox* checks_[6][STAT_COUNT] = {}; // [angle][stat]

    std::unique_ptr<PlaybackManager> playback_;
    std::string loadedCsv_;
    std::string tempPdfPath_;
};
