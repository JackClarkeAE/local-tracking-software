#include "info_tab.h"
#include "../camera/camera_types.h"
#include "../biofeedback/biofeedback_engine.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>
#include <QScrollArea>

InfoTab::InfoTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    root->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* layout = new QVBoxLayout(content);
    layout->setSpacing(16);

    // Heading
    auto* heading = new QLabel("<h2>LOCAL TRACKING SOFTWARE (Qt)</h2>");
    layout->addWidget(heading);

    // === Joint Index Reference ===
    {
        auto* groupTitle = new QLabel("<h3>Joint Position Index Reference</h3>");
        layout->addWidget(groupTitle);
        auto* note = new QLabel("<i>Both ZED and Azure Kinect are mapped to this canonical 32-joint skeleton.</i>");
        note->setStyleSheet("color: #999;");
        layout->addWidget(note);

        auto* rowLayout = new QHBoxLayout;

        auto makeJointTable = [](int startIdx, int endIdx) -> QTableWidget* {
            int n = endIdx - startIdx;
            auto* table = new QTableWidget(n, 2);
            table->setHorizontalHeaderLabels({"Index", "Joint Name"});
            table->horizontalHeader()->setStretchLastSection(true);
            table->verticalHeader()->setVisible(false);
            table->verticalHeader()->setDefaultSectionSize(28);
            table->verticalHeader()->setMinimumSectionSize(26);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->setSelectionMode(QAbstractItemView::NoSelection);
            table->setFixedHeight(n * 28 + 38);
            for (int i = startIdx; i < endIdx; i++) {
                int row = i - startIdx;
                table->setItem(row, 0, new QTableWidgetItem(QString::number(i)));
                table->setItem(row, 1, new QTableWidgetItem(jointName(i)));
            }
            table->setColumnWidth(0, 50);
            return table;
        };

        rowLayout->addWidget(makeJointTable(0, 16));
        rowLayout->addWidget(makeJointTable(16, JOINT_COUNT));
        layout->addLayout(rowLayout);
    }

    // === Bone Connections ===
    {
        auto* groupTitle = new QLabel("<h3>Bone Connections</h3>");
        layout->addWidget(groupTitle);
        auto* note = new QLabel("<i>Pairs of joint indices connected by bones:</i>");
        note->setStyleSheet("color: #999;");
        layout->addWidget(note);

        auto addBoneLine = [&](const QString& region, const QString& bones, const QString& color) {
            auto* label = new QLabel(QString("<b>%1:</b> <span style='color:%2;'>%3</span>")
                                     .arg(region).arg(color).arg(bones));
            layout->addWidget(label);
        };

        addBoneLine("Spine", "0-1, 1-2, 2-3, 3-26", "#60c080");
        addBoneLine("Left Arm", "2-4, 4-5, 5-6, 6-7, 7-8, 8-9, 7-10", "#8080e0");
        addBoneLine("Right Arm", "2-11, 11-12, 12-13, 13-14, 14-15, 15-16, 14-17", "#e08080");
        addBoneLine("Left Leg", "0-18, 18-19, 19-20, 20-21", "#8080e0");
        addBoneLine("Right Leg", "0-22, 22-23, 23-24, 24-25", "#e08080");
        addBoneLine("Face", "26-27, 27-28, 27-30, 28-29, 30-31", "#e0e080");
    }

    // === Biomechanical Angles ===
    {
        auto* groupTitle = new QLabel("<h3>Biomechanical Angle Definitions</h3>");
        layout->addWidget(groupTitle);
        auto* note = new QLabel("<i>Angles used in biofeedback transforms and measurements:</i>");
        note->setStyleSheet("color: #999;");
        layout->addWidget(note);

        auto* table = new QTableWidget(biomechAngleCount(), 4);
        table->setHorizontalHeaderLabels({"Angle", "Proximal", "Vertex", "Distal"});
        table->horizontalHeader()->setStretchLastSection(true);
        table->verticalHeader()->setVisible(false);
        table->verticalHeader()->setDefaultSectionSize(28);
        table->verticalHeader()->setMinimumSectionSize(26);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::NoSelection);
        table->setFixedHeight(biomechAngleCount() * 28 + 38);

        for (int i = 0; i < biomechAngleCount(); i++) {
            const auto& def = getAngleDefinition((BiomechAngle)i);
            table->setItem(i, 0, new QTableWidgetItem(def.displayName));
            table->setItem(i, 1, new QTableWidgetItem(QString("%1 (%2)").arg(jointName(def.proximalJoint)).arg(def.proximalJoint)));
            table->setItem(i, 2, new QTableWidgetItem(QString("%1 (%2)").arg(jointName(def.vertexJoint)).arg(def.vertexJoint)));
            table->setItem(i, 3, new QTableWidgetItem(QString("%1 (%2)").arg(jointName(def.distalJoint)).arg(def.distalJoint)));
        }
        layout->addWidget(table);
    }

    // === CSV format ===
    {
        auto* groupTitle = new QLabel("<h3>CSV Recording Format</h3>");
        layout->addWidget(groupTitle);
        auto* note = new QLabel("<i>Joint position CSV columns:</i>");
        note->setStyleSheet("color: #999;");
        layout->addWidget(note);

        auto* bullets = new QLabel(
            "<ul>"
            "<li><b>time_seconds</b> &mdash; seconds since recording start</li>"
            "<li><b>body_id</b> &mdash; tracked person ID</li>"
            "<li><b>joint_index</b> &mdash; 0-31 (see table above)</li>"
            "<li><b>joint_name</b> &mdash; human-readable name</li>"
            "<li><b>x, y, z</b> &mdash; 3D position in metres</li>"
            "<li><b>confidence</b> &mdash; tracking confidence 0.0-1.0</li>"
            "</ul>");
        layout->addWidget(bullets);

        auto* binLabel = new QLabel("<i>Binary angle export format (8 bytes):</i>");
        binLabel->setStyleSheet("color: #999;");
        layout->addWidget(binLabel);
        auto* binBullets = new QLabel(
            "<ul>"
            "<li><b>float kneeFlexion</b> &mdash; degrees, 0 = straight</li>"
            "<li><b>float confidence</b> &mdash; 0.0-1.0, average of hip/knee/ankle joints</li>"
            "</ul>");
        layout->addWidget(binBullets);
    }

    layout->addStretch();
}
