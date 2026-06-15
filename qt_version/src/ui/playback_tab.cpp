#include "playback_tab.h"
#include "skeleton_view.h"
#include "experimental_tab.h"
#include "widget_kit.h"
#include "../app_controller.h"
#include "../recording/playback.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSlider>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QOpenGLWidget>
#include <QRadioButton>
#include <QFileDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QCheckBox>
#include <QButtonGroup>
#include <QFrame>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStringList>
#include <algorithm>

static QWidget* buttonRow(QWidget* left, QWidget* right = nullptr) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(left, 1);
    if (right) layout->addWidget(right, 1);
    return row;
}

static QVBoxLayout* sectionLayout(QWidget* section, const QString& title) {
    auto* layout = new QVBoxLayout(section);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);
    layout->addWidget(WidgetKit::sectionHeader(title));
    return layout;
}

class FlagSlider : public QSlider {
public:
    explicit FlagSlider(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QSlider(orientation, parent) {
        setMinimumHeight(46);
    }

    void setFlagData(const std::vector<PlaybackFlag>& flags, double duration) {
        flags_ = flags;
        duration_ = duration;
        update();
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        QSlider::paintEvent(event);
        if (flags_.empty() || duration_ <= 0.0 || orientation() != Qt::Horizontal) return;

        QStyleOptionSlider opt;
        initStyleOption(&opt);
        const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt,
                                                     QStyle::SC_SliderGroove, this);
        if (!groove.isValid() || groove.width() <= 0) return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor bg(20, 20, 25, 210);
        const QColor colors[] = {
            QColor(255, 150, 150, 220),
            QColor(150, 200, 255, 220),
            QColor(150, 255, 150, 220),
            QColor(255, 220, 130, 220),
            QColor(200, 150, 255, 220),
            QColor(255, 180, 220, 220),
        };
        const int colorCount = (int)(sizeof(colors) / sizeof(colors[0]));

        QFont labelFont = font();
        labelFont.setPointSize(std::max(7, labelFont.pointSize() - 1));
        painter.setFont(labelFont);
        const QFontMetrics fm(labelFont);

        for (int i = 0; i < (int)flags_.size(); ++i) {
            const auto& flag = flags_[i];
            if (flag.timeSeconds < 0.0 || flag.timeSeconds > duration_) continue;

            const double ratio = flag.timeSeconds / duration_;
            const int x = groove.left() + (int)(ratio * groove.width());
            const QColor color = colors[i % colorCount];

            painter.setPen(QPen(color, 2));
            painter.drawLine(QPoint(x, 4), QPoint(x, groove.bottom() + 4));

            const QString label = QString::fromStdString(flag.label);
            QRect textRect = fm.boundingRect(label).adjusted(-3, -1, 3, 1);
            int tx = x - textRect.width() / 2;
            tx = std::clamp(tx, 0, std::max(0, width() - textRect.width()));
            textRect.moveTopLeft(QPoint(tx, 1));
            painter.setPen(Qt::NoPen);
            painter.setBrush(bg);
            painter.drawRoundedRect(textRect, 2, 2);
            painter.setPen(color);
            painter.drawText(textRect, Qt::AlignCenter, label);
        }
    }

private:
    std::vector<PlaybackFlag> flags_;
    double duration_ = 0.0;
};

PlaybackTab::PlaybackTab(AppController* ctrl, ExperimentalTab* exp, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl), exp_(exp) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);

    // Top section: splitter (viewport | controls)
    auto* topSplit = new QSplitter(Qt::Horizontal);
    topSplit->setHandleWidth(5);

    // Left: viewport(s) — single view by default, becomes split-screen on compare mode
    auto* viewContainer = new QFrame;
    viewContainer->setFrameShape(QFrame::NoFrame);
    viewContainer->setProperty("dsPanel", true);
    auto* viewLayout = new QHBoxLayout(viewContainer);
    viewLayout->setContentsMargins(6, 6, 6, 6);
    viewLayout->setSpacing(6);
    viewMain_ = new SkeletonView;
    viewMain_->setMode(SkeletonView::Mode::Orbit3D);
    connect(viewMain_, &QOpenGLWidget::frameSwapped,
            this, &PlaybackTab::onViewportFrameSwapped,
            Qt::QueuedConnection);
    viewLayout->addWidget(viewMain_);
    viewSecondary_ = new SkeletonView;
    viewSecondary_->setMode(SkeletonView::Mode::Orbit3D);
    viewSecondary_->setVisible(false);
    viewLayout->addWidget(viewSecondary_);
    topSplit->addWidget(viewContainer);

    // Right: controls. The panel scrolls only when the window is genuinely small.
    auto* ctrlScroll = new QScrollArea;
    ctrlScroll->setWidgetResizable(true);
    ctrlScroll->setFrameShape(QFrame::NoFrame);
    ctrlScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* ctrlContent = WidgetKit::panel("PLAYBACK");
    ctrlContent->setProperty("controlsPanel", true);
    ctrlScroll->setWidget(ctrlContent);
    auto* cl = static_cast<QVBoxLayout*>(ctrlContent->layout());
    cl->setContentsMargins(9, 8, 9, 9);
    cl->setSpacing(6);

    // File 1
    WidgetKit::addSectionHeader(cl, "File 1");
    path1Edit_ = new QLineEdit;
    cl->addWidget(path1Edit_);
    browse1Btn_ = WidgetKit::button("Browse...");
    load1Btn_ = WidgetKit::button("Load File", WidgetKit::ButtonRole::Primary);
    loadLegacy1Btn_ = WidgetKit::button("Load Legacy Data");
    loadResampled1Btn_ = WidgetKit::button("Load Resampled Data");
    connect(browse1Btn_, &QPushButton::clicked, this, &PlaybackTab::onBrowse1);
    connect(load1Btn_, &QPushButton::clicked, this, &PlaybackTab::onLoadFile1);
    connect(loadLegacy1Btn_, &QPushButton::clicked, this, &PlaybackTab::onLoadLegacy1);
    connect(loadResampled1Btn_, &QPushButton::clicked, this, &PlaybackTab::onLoadResampled1);
    cl->addWidget(buttonRow(browse1Btn_, load1Btn_));
    cl->addWidget(buttonRow(loadLegacy1Btn_, loadResampled1Btn_));
    file1InfoLabel_ = new QLabel;
    file1InfoLabel_->setProperty("muted", true);
    cl->addWidget(file1InfoLabel_);

    // Comparison mode
    WidgetKit::addSectionHeader(cl, "Comparison");
    auto* cmpRow = new QWidget;
    cmpRow->setProperty("segmented", true);
    auto* cmpLayout = new QHBoxLayout(cmpRow);
    cmpLayout->setContentsMargins(7, 2, 7, 2);
    cmpLayout->setSpacing(14);
    modeSingleRb_ = new QRadioButton("Single");
    modeSplitRb_ = new QRadioButton("Split");
    modeOverlayRb_ = new QRadioButton("Overlay");
    modeSingleRb_->setChecked(true);
    auto* cmpGrp = new QButtonGroup(this);
    cmpGrp->addButton(modeSingleRb_);
    cmpGrp->addButton(modeSplitRb_);
    cmpGrp->addButton(modeOverlayRb_);
    connect(modeSingleRb_, &QRadioButton::toggled, this, &PlaybackTab::onCompareModeChanged);
    connect(modeSplitRb_, &QRadioButton::toggled, this, &PlaybackTab::onCompareModeChanged);
    connect(modeOverlayRb_, &QRadioButton::toggled, this, &PlaybackTab::onCompareModeChanged);
    cmpLayout->addWidget(modeSingleRb_);
    cmpLayout->addWidget(modeSplitRb_);
    cmpLayout->addWidget(modeOverlayRb_);
    cmpLayout->addStretch();
    cl->addWidget(cmpRow);

    // File 2
    file2Section_ = new QWidget;
    auto* file2Layout = sectionLayout(file2Section_, "File 2");
    path2Edit_ = new QLineEdit;
    file2Layout->addWidget(path2Edit_);
    browse2Btn_ = WidgetKit::button("Browse...");
    load2Btn_ = WidgetKit::button("Load File", WidgetKit::ButtonRole::Primary);
    loadLegacy2Btn_ = WidgetKit::button("Load Legacy Data");
    loadResampled2Btn_ = WidgetKit::button("Load Resampled Data");
    connect(browse2Btn_, &QPushButton::clicked, this, &PlaybackTab::onBrowse2);
    connect(load2Btn_, &QPushButton::clicked, this, &PlaybackTab::onLoadFile2);
    connect(loadLegacy2Btn_, &QPushButton::clicked, this, &PlaybackTab::onLoadLegacy2);
    connect(loadResampled2Btn_, &QPushButton::clicked, this, &PlaybackTab::onLoadResampled2);
    file2Layout->addWidget(buttonRow(browse2Btn_, load2Btn_));
    file2Layout->addWidget(buttonRow(loadLegacy2Btn_, loadResampled2Btn_));
    file2InfoLabel_ = new QLabel;
    file2InfoLabel_->setProperty("muted", true);
    file2Layout->addWidget(file2InfoLabel_);
    file2Section_->setVisible(false);
    cl->addWidget(file2Section_);

    // Sync
    syncSection_ = new QWidget;
    auto* syncLayout = sectionLayout(syncSection_, "Sync");
    auto* offRow = new QHBoxLayout;
    offRow->setContentsMargins(0, 0, 0, 0);
    offRow->setSpacing(6);
    offRow->addWidget(new QLabel("Time Offset:"));
    timeOffsetSpin_ = new QDoubleSpinBox;
    timeOffsetSpin_->setRange(-3600, 3600);
    timeOffsetSpin_->setDecimals(1);
    timeOffsetSpin_->setSuffix(" s");
    connect(timeOffsetSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v) { ctrl_->compTimeOffset = v; });
    offRow->addWidget(timeOffsetSpin_, 1);
    syncLayout->addLayout(offRow);
    syncSection_->setVisible(false);
    cl->addWidget(syncSection_);

    // Playback controls
    WidgetKit::addSectionHeader(cl, "Controls");
    auto* pbWidget = new QWidget;
    auto* pbLayout = new QVBoxLayout(pbWidget);
    pbLayout->setContentsMargins(0, 0, 0, 0);
    pbLayout->setSpacing(6);
    auto* playRow = new QHBoxLayout;
    playRow->setContentsMargins(0, 0, 0, 0);
    playRow->setSpacing(6);
    playBtn_ = WidgetKit::button("Play", WidgetKit::ButtonRole::Primary);
    resetBtn_ = WidgetKit::button("Reset");
    connect(playBtn_, &QPushButton::clicked, this, &PlaybackTab::onPlayPause);
    connect(resetBtn_, &QPushButton::clicked, this, &PlaybackTab::onReset);
    playRow->addWidget(playBtn_);
    playRow->addWidget(resetBtn_);
    pbLayout->addLayout(playRow);

    auto* frameRow = new QHBoxLayout;
    frameRow->setContentsMargins(0, 0, 0, 0);
    frameRow->setSpacing(6);
    frameBackBtn_ = WidgetKit::button("<< Frame");
    frameFwdBtn_ = WidgetKit::button("Frame >>");
    connect(frameBackBtn_, &QPushButton::clicked, this, &PlaybackTab::onFrameBack);
    connect(frameFwdBtn_, &QPushButton::clicked, this, &PlaybackTab::onFrameForward);
    frameRow->addWidget(frameBackBtn_);
    frameRow->addWidget(frameFwdBtn_);
    pbLayout->addLayout(frameRow);

    auto* spdRow = new QHBoxLayout;
    spdRow->setContentsMargins(0, 0, 0, 0);
    spdRow->setSpacing(8);
    spdRow->addWidget(new QLabel("Speed:"));
    speedSlider_ = new QSlider(Qt::Horizontal);
    speedSlider_->setRange(10, 500);
    speedSlider_->setValue(100);
    speedSlider_->setSingleStep(10);
    speedSlider_->setPageStep(25);
    speedValueLabel_ = new QLabel("1.0x");
    speedValueLabel_->setMinimumWidth(42);
    connect(speedSlider_, &QSlider::valueChanged, this, [this](int value) {
        playbackSpeed_ = value / 100.0f;
        speedValueLabel_->setText(QString("%1x").arg(playbackSpeed_, 0, 'f', 1));
    });
    spdRow->addWidget(speedSlider_, 1);
    spdRow->addWidget(speedValueLabel_);
    pbLayout->addLayout(spdRow);

    cl->addWidget(pbWidget);

    // 3D view controls hint
    WidgetKit::addSectionHeader(cl, "3D View");
    auto* hintWidget = new QWidget;
    auto* hintLayout = new QVBoxLayout(hintWidget);
    hintLayout->setContentsMargins(0, 0, 0, 0);
    hintLayout->setSpacing(6);
    auto* hintLabel = new QLabel("Left drag: rotate\nRight drag: pan\nScroll: zoom\nMiddle-click: reset");
    hintLabel->setProperty("muted", true);
    hintLayout->addWidget(hintLabel);
    auto* resetViewBtn = WidgetKit::button("Reset View");
    connect(resetViewBtn, &QPushButton::clicked, this, [this]() {
        viewMain_->camera().reset();
        viewMain_->update();
        viewSecondary_->camera().reset();
        viewSecondary_->update();
    });
    hintLayout->addWidget(resetViewBtn);
    cl->addWidget(hintWidget);

    WidgetKit::addSectionHeader(cl, "Info");
    infoLabel_ = new QLabel("No recording loaded.");
    infoLabel_->setWordWrap(true);
    infoLabel_->setProperty("muted", true);
    infoLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    cl->addWidget(infoLabel_);

    cl->addStretch();

    topSplit->addWidget(ctrlScroll);
    topSplit->setStretchFactor(0, 3);
    topSplit->setStretchFactor(1, 1);
    topSplit->setSizes({1100, 390});
    root->addWidget(topSplit, 1);

    // Bottom: seek bar
    auto* seekRow = new QHBoxLayout;
    timeLabel_ = new QLabel("0.0 s");
    timeLabel_->setMinimumWidth(60);
    seekSlider_ = new FlagSlider(Qt::Horizontal);
    seekSlider_->setRange(0, 1000);
    connect(seekSlider_, &QSlider::sliderMoved, this, &PlaybackTab::onSeek);
    durationLabel_ = new QLabel("0.0 s");
    durationLabel_->setMinimumWidth(60);
    seekRow->addWidget(timeLabel_);
    seekRow->addWidget(seekSlider_, 1);
    seekRow->addWidget(durationLabel_);
    root->addLayout(seekRow);

    // Initial state
    loadLegacy1Btn_->setVisible(false);
    loadLegacy2Btn_->setVisible(false);
    loadResampled1Btn_->setVisible(false);
    loadResampled2Btn_->setVisible(false);

    // Listen to experimental toggles
    if (exp_) {
        connect(exp_, &ExperimentalTab::legacyPlaybackToggled, this, &PlaybackTab::onLegacyToggled);
        connect(exp_, &ExperimentalTab::resampledPlaybackToggled, this, &PlaybackTab::onResampledToggled);
    }

    playbackClock_.start();
    lastPlaybackNsec_ = playbackClock_.nsecsElapsed();
}

void PlaybackTab::onBrowse1() {
    QString path = QFileDialog::getOpenFileName(this, "Load Recording",
        QString::fromStdString(ctrl_->config().recordingsDir), "CSV Files (*.csv)");
    if (!path.isEmpty()) path1Edit_->setText(path);
}

void PlaybackTab::onLoadFile1() {
    if (path1Edit_->text().isEmpty()) return;
    if (ctrl_->playback().loadCSV(path1Edit_->text().toStdString())) {
        playing_ = false;
        playbackTime_ = 0;
        file1InfoLabel_->setText(QString("Frames: %1  Duration: %2 s")
            .arg(ctrl_->playback().frameCount())
            .arg(ctrl_->playback().duration(), 0, 'f', 1));
        viewMain_->camera().reset();
        updateFrame();
        updateSeekBar();
        updateInfoPanel();
    } else {
        QMessageBox::warning(this, "Load", "Failed to load recording file.");
    }
}

void PlaybackTab::onBrowse2() {
    QString path = QFileDialog::getOpenFileName(this, "Load Comparison File",
        QString::fromStdString(ctrl_->config().recordingsDir), "CSV Files (*.csv)");
    if (!path.isEmpty()) path2Edit_->setText(path);
}

void PlaybackTab::onLoadFile2() {
    if (path2Edit_->text().isEmpty()) return;
    if (ctrl_->compPlayback().loadCSV(path2Edit_->text().toStdString())) {
        file2InfoLabel_->setText(QString("Frames: %1  Duration: %2 s")
            .arg(ctrl_->compPlayback().frameCount())
            .arg(ctrl_->compPlayback().duration(), 0, 'f', 1));
        updateFrame();
        updateSeekBar();
        updateInfoPanel();
    } else {
        QMessageBox::warning(this, "Load", "Failed to load comparison file.");
    }
}

void PlaybackTab::onLoadLegacy1() {
    QString path = QFileDialog::getOpenFileName(this, "Load Legacy JSON", "",
        "JSON Files (*.json)");
    if (path.isEmpty()) return;
    if (ctrl_->playback().loadLegacyJSON(path.toStdString())) {
        path1Edit_->setText(path);
        playing_ = false; playbackTime_ = 0;
        file1InfoLabel_->setText(QString("Frames: %1  Duration: %2 s")
            .arg(ctrl_->playback().frameCount())
            .arg(ctrl_->playback().duration(), 0, 'f', 1));
        viewMain_->camera().reset();
        updateFrame();
        updateSeekBar();
        updateInfoPanel();
    } else {
        QMessageBox::warning(this, "Load", "Failed to load legacy JSON file.");
    }
}

void PlaybackTab::onLoadLegacy2() {
    QString path = QFileDialog::getOpenFileName(this, "Load Legacy JSON (File 2)", "",
        "JSON Files (*.json)");
    if (path.isEmpty()) return;
    if (ctrl_->compPlayback().loadLegacyJSON(path.toStdString())) {
        path2Edit_->setText(path);
        file2InfoLabel_->setText(QString("Frames: %1  Duration: %2 s")
            .arg(ctrl_->compPlayback().frameCount())
            .arg(ctrl_->compPlayback().duration(), 0, 'f', 1));
        updateFrame();
        updateSeekBar();
        updateInfoPanel();
    } else {
        QMessageBox::warning(this, "Load", "Failed to load legacy JSON file (File 2).");
    }
}

void PlaybackTab::onLoadResampled1() {
    QString path = QFileDialog::getOpenFileName(this, "Load Resampled CSV", "",
        "CSV Files (*.csv)");
    if (path.isEmpty()) return;
    if (ctrl_->playback().loadResampledCSV(path.toStdString())) {
        path1Edit_->setText(path);
        playing_ = false; playbackTime_ = 0;
        file1InfoLabel_->setText(QString("Frames: %1  Duration: %2 s")
            .arg(ctrl_->playback().frameCount())
            .arg(ctrl_->playback().duration(), 0, 'f', 1));
        viewMain_->camera().reset();
        updateFrame();
        updateSeekBar();
        updateInfoPanel();
    } else {
        QMessageBox::warning(this, "Load", "Failed to load resampled CSV.");
    }
}

void PlaybackTab::onLoadResampled2() {
    QString path = QFileDialog::getOpenFileName(this, "Load Resampled CSV (File 2)", "",
        "CSV Files (*.csv)");
    if (path.isEmpty()) return;
    if (ctrl_->compPlayback().loadResampledCSV(path.toStdString())) {
        path2Edit_->setText(path);
        file2InfoLabel_->setText(QString("Frames: %1  Duration: %2 s")
            .arg(ctrl_->compPlayback().frameCount())
            .arg(ctrl_->compPlayback().duration(), 0, 'f', 1));
        updateFrame();
        updateSeekBar();
        updateInfoPanel();
    } else {
        QMessageBox::warning(this, "Load", "Failed to load resampled CSV (File 2).");
    }
}

void PlaybackTab::onPlayPause() {
    playing_ = !playing_;
    playBtn_->setText(playing_ ? "Pause" : "Play");
    if (playing_ && playbackTime_ >= ctrl_->playback().duration()) playbackTime_ = 0;
    lastPlaybackNsec_ = playbackClock_.nsecsElapsed();
    lastControlsRefreshNsec_ = 0;
    if (playing_) {
        updateFrame(false);
        updateSeekBar();
        updateInfoPanel();
    }
}

void PlaybackTab::onReset() {
    playing_ = false;
    playBtn_->setText("Play");
    playbackTime_ = 0;
    updateFrame();
    updateSeekBar();
}

void PlaybackTab::onFrameBack() {
    playing_ = false;
    playBtn_->setText("Play");
    int idx = ctrl_->playback().getFrameIndexAtTime(playbackTime_);
    if (idx > 0) {
        auto* p = ctrl_->playback().getFrame(idx - 1);
        if (p) {
            playbackTime_ = p->timeSeconds;
            updateFrame();
            updateSeekBar();
        }
    }
}

void PlaybackTab::onFrameForward() {
    playing_ = false;
    playBtn_->setText("Play");
    int idx = ctrl_->playback().getFrameIndexAtTime(playbackTime_);
    if (idx < ctrl_->playback().frameCount() - 1) {
        auto* p = ctrl_->playback().getFrame(idx + 1);
        if (p) {
            playbackTime_ = p->timeSeconds;
            updateFrame();
            updateSeekBar();
        }
    }
}

void PlaybackTab::onCompareModeChanged() {
    if (modeSingleRb_->isChecked()) {
        ctrl_->compareMode = CompareMode::Single;
        viewSecondary_->setVisible(false);
    } else if (modeSplitRb_->isChecked()) {
        ctrl_->compareMode = CompareMode::SplitScreen;
        viewSecondary_->setVisible(true);
    } else {
        ctrl_->compareMode = CompareMode::Overlay;
        viewSecondary_->setVisible(false);
    }
    bool showCmp = (ctrl_->compareMode != CompareMode::Single);
    if (file2Section_) file2Section_->setVisible(showCmp);
    if (syncSection_) syncSection_->setVisible(showCmp);
    updateFrame();
}

void PlaybackTab::onSeek(int value) {
    double d = ctrl_->playback().duration();
    if (ctrl_->compareMode != CompareMode::Single && ctrl_->compPlayback().isLoaded()) {
        double compEnd = ctrl_->compPlayback().duration() - ctrl_->compTimeOffset;
        if (compEnd > d) d = compEnd;
    }
    playbackTime_ = (value / 1000.0) * d;
    updateFrame();
    timeLabel_->setText(QString("%1 s").arg(playbackTime_, 0, 'f', 1));
}

void PlaybackTab::updateSeekBar() {
    double d = ctrl_->playback().duration();
    if (ctrl_->compareMode != CompareMode::Single && ctrl_->compPlayback().isLoaded()) {
        double compEnd = ctrl_->compPlayback().duration() - ctrl_->compTimeOffset;
        if (compEnd > d) d = compEnd;
    }
    durationLabel_->setText(QString("%1 s").arg(d, 0, 'f', 1));
    timeLabel_->setText(QString("%1 s").arg(playbackTime_, 0, 'f', 1));
    seekSlider_->blockSignals(true);
    seekSlider_->setValue(d > 0 ? (int)(playbackTime_ / d * 1000) : 0);
    seekSlider_->blockSignals(false);
    if (auto* flags = dynamic_cast<FlagSlider*>(seekSlider_)) {
        flags->setFlagData(ctrl_->playback().flags(), d);
    }
}

void PlaybackTab::onViewportFrameSwapped() {
    if (!playing_ || !ctrl_->playback().isLoaded()) return;

    const qint64 nowNsec = playbackClock_.nsecsElapsed();
    double deltaSeconds = static_cast<double>(nowNsec - lastPlaybackNsec_) / 1000000000.0;
    lastPlaybackNsec_ = nowNsec;
    if (deltaSeconds < 0.0) deltaSeconds = 0.0;

    playbackTime_ += deltaSeconds * playbackSpeed_;

    double maxDur = ctrl_->playback().duration();
    if (ctrl_->compareMode != CompareMode::Single && ctrl_->compPlayback().isLoaded()) {
        double compEnd = ctrl_->compPlayback().duration() - ctrl_->compTimeOffset;
        if (compEnd > maxDur) maxDur = compEnd;
    }
    if (playbackTime_ > maxDur) {
        playbackTime_ = maxDur;
        playing_ = false;
        playBtn_->setText("Play");
        updateFrame();
        updateSeekBar();
        return;
    }

    updateFrame(false);

    if (lastControlsRefreshNsec_ == 0 || nowNsec - lastControlsRefreshNsec_ >= 50000000) {
        updateSeekBar();
        updateInfoPanel();
        lastControlsRefreshNsec_ = nowNsec;
    }
}

void PlaybackTab::updateFrame(bool updateInfo) {
    if (!ctrl_->playback().isLoaded()) {
        if (updateInfo) updateInfoPanel();
        return;
    }

    const PlaybackFrame* pf = ctrl_->playback().getFrameAtTime(playbackTime_);
    auto fd = std::make_shared<FrameData>();
    if (pf) fd->bodies = pf->bodies;

    if (ctrl_->compareMode == CompareMode::Overlay && ctrl_->compPlayback().isLoaded()) {
        auto fd2 = std::make_shared<FrameData>();
        double compTime = playbackTime_ + ctrl_->compTimeOffset;
        const PlaybackFrame* pf2 = ctrl_->compPlayback().getFrameAtTime(compTime);
        if (pf2) fd2->bodies = pf2->bodies;
        viewMain_->setOverlayFrames(fd, fd2);
    } else if (ctrl_->compareMode == CompareMode::SplitScreen && ctrl_->compPlayback().isLoaded()) {
        viewMain_->clearOverlay();
        viewMain_->setFrame(fd);
        auto fd2 = std::make_shared<FrameData>();
        double compTime = playbackTime_ + ctrl_->compTimeOffset;
        const PlaybackFrame* pf2 = ctrl_->compPlayback().getFrameAtTime(compTime);
        if (pf2) fd2->bodies = pf2->bodies;
        viewSecondary_->setFrame(fd2);
    } else {
        viewMain_->clearOverlay();
        viewMain_->setFrame(fd);
    }
    if (updateInfo) updateInfoPanel();
}

void PlaybackTab::updateInfoPanel() {
    if (!infoLabel_) return;
    if (!ctrl_->playback().isLoaded()) {
        infoLabel_->setText("No recording loaded.");
        return;
    }

    QStringList lines;
    const PlaybackFrame* pf = ctrl_->playback().getFrameAtTime(playbackTime_);
    if (pf) {
        lines << QString("F1: %1 s  Bodies: %2")
            .arg(pf->timeSeconds, 0, 'f', 3)
            .arg((int)pf->bodies.size());
    }

    if (ctrl_->compareMode != CompareMode::Single && ctrl_->compPlayback().isLoaded()) {
        double compTime = playbackTime_ + ctrl_->compTimeOffset;
        const PlaybackFrame* pf2 = ctrl_->compPlayback().getFrameAtTime(compTime);
        if (pf2) {
            lines << QString("F2: %1 s  Bodies: %2")
                .arg(pf2->timeSeconds, 0, 'f', 3)
                .arg((int)pf2->bodies.size());
        }
    }

    const auto& metadata = ctrl_->playback().metadata();
    if (metadata.loaded) {
        lines << "";
        if (!metadata.patientId.empty())
            lines << QString("Patient: %1").arg(QString::fromStdString(metadata.patientId));
        if (!metadata.protocol.empty())
            lines << QString("Protocol: %1").arg(QString::fromStdString(metadata.protocol));
        if (!metadata.date.empty())
            lines << QString("Date: %1").arg(QString::fromStdString(metadata.date));
        if (!metadata.cameraNames.empty()) {
            QStringList cams;
            for (const auto& camera : metadata.cameraNames)
                cams << QString::fromStdString(camera);
            lines << QString("Camera: %1").arg(cams.join(", "));
        }
        lines << QString("Biofeedback: %1").arg(metadata.biofeedbackActive ? "Active" : "Inactive");
    }

    const auto& flags = ctrl_->playback().flags();
    lines << "";
    lines << QString("Flags: %1").arg((int)flags.size());
    if (!flags.empty()) {
        const PlaybackFlag* current = nullptr;
        const PlaybackFlag* next = nullptr;
        for (const auto& flag : flags) {
            if (flag.timeSeconds <= playbackTime_) current = &flag;
            else {
                next = &flag;
                break;
            }
        }
        if (current) {
            lines << QString("Current: %1 @ %2 s")
                .arg(QString::fromStdString(current->label))
                .arg(current->timeSeconds, 0, 'f', 1);
        }
        if (next) {
            lines << QString("Next: %1 @ %2 s")
                .arg(QString::fromStdString(next->label))
                .arg(next->timeSeconds, 0, 'f', 1);
        }
    }

    infoLabel_->setText(lines.join("\n"));
}

void PlaybackTab::onLegacyToggled(bool enabled) {
    loadLegacy1Btn_->setVisible(enabled);
    loadLegacy2Btn_->setVisible(enabled);
}

void PlaybackTab::onResampledToggled(bool enabled) {
    loadResampled1Btn_->setVisible(enabled);
    loadResampled2Btn_->setVisible(enabled);
}
