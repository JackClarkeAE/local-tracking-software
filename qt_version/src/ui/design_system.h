#pragma once

class QApplication;

namespace DesignSystem {

enum class Theme {
    ClinicalSlate,  // upstream dark theme (research UI)
    Clinical        // light clinical theme (YCTS clinical UI layer)
};

void apply(QApplication& app, Theme theme = Theme::Clinical);

}
