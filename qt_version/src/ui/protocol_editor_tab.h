#pragma once
#include "../recording/protocol.h"
#include <QWidget>
#include <vector>

class AppController;
class QLineEdit;
class QTableWidget;
class QPushButton;
class QLabel;
class QListWidget;
class QDoubleSpinBox;

class ProtocolEditorTab : public QWidget {
    Q_OBJECT
public:
    explicit ProtocolEditorTab(AppController* ctrl, QWidget* parent = nullptr);

private slots:
    void onAddEvent();
    void onSortByTime();
    void onClearAll();
    void onSaveProtocol();
    void onSaveAs();
    void onLoadProtocol();
    void onValidate();
    void onRefreshList();
    void onSavedProtocolSelected();

private:
    void rebuildEventTable();
    void rebuildSavedList();
    void refreshTimeSpins(int excludeRow);

    AppController* ctrl_;
    Protocol editing_;

    QLineEdit* nameEdit_ = nullptr;
    QTableWidget* table_ = nullptr;
    QListWidget* savedList_ = nullptr;
    QLabel* validationLabel_ = nullptr;

    // Time display mode: absolute (default) or delay since previous event.
    // Stored times are always absolute — only display/editing converts.
    bool relativeTime_ = false;
    std::vector<QDoubleSpinBox*> timeSpins_;
};
