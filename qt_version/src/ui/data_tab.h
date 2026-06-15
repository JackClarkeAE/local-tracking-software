#pragma once
#include <QWidget>

class AppController;
class QLineEdit;
class QComboBox;
class QTableWidget;
class QLabel;
class QPushButton;
class QTimer;
class QProgressBar;

class DataTab : public QWidget {
    Q_OBJECT
public:
    explicit DataTab(AppController* ctrl, QWidget* parent = nullptr);

private slots:
    void refresh();
    void onFilterChanged();
    void onSelectionChanged();
    void onSelectAll();
    void onDeselectAll();
    void onDelete();
    void onRename();
    void onSaveMetadata();
    void onExport();
    void onComputeStats();
    void onStatsTick();

private:
    void rebuildTable();
    void rebuildProtocolFilter();
    void updateDetailPanel();
    int currentDetailRow() const;

    AppController* ctrl_;

    QLineEdit* filterEdit_ = nullptr;
    QComboBox* protocolFilter_ = nullptr;
    QPushButton* selectAllBtn_ = nullptr;
    QPushButton* deselectBtn_ = nullptr;
    QPushButton* deleteBtn_ = nullptr;
    QPushButton* refreshBtn_ = nullptr;

    QTableWidget* table_ = nullptr;

    // Detail panel
    QLineEdit* renameEdit_ = nullptr;
    QPushButton* renameBtn_ = nullptr;
    QLabel* metaInfoLabel_ = nullptr;
    QLineEdit* patientIdEdit_ = nullptr;
    QLineEdit* operatorEdit_ = nullptr;
    QLineEdit* notesEdit_ = nullptr;
    QPushButton* saveMetaBtn_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QPushButton* computeStatsBtn_ = nullptr;
    QProgressBar* statsProgress_ = nullptr;
    QTableWidget* anglesTable_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    // Export
    QComboBox* formatCombo_ = nullptr;
    QComboBox* batchModeCombo_ = nullptr;
    QPushButton* exportBtn_ = nullptr;
    QProgressBar* exportProgress_ = nullptr;
    QLabel* exportStatus_ = nullptr;

    QTimer* statsTimer_ = nullptr;
    QTimer* exportTimer_ = nullptr;

    int detailIdx_ = -1;
    int deleteConfirmStep_ = 0;
    float deleteConfirmTimer_ = 0;
};
