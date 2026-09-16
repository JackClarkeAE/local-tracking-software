#ifdef _WIN32
#include <windows.h>
#endif

#include <QApplication>
#include <QIcon>
#include <QStringList>
#include "ui/main_window.h"
#include "ui/design_system.h"
#include "ui/config_setup_dialog.h"
#include "ui/app_ui_mode.h"
#include "app_controller.h"

// Which UI layer to show. Command line wins over config.ini so a clinic
// install can be launched into the research UI for troubleshooting without
// editing the config:  YCTS_Qt --research   |   YCTS_Qt --clinical
static UiMode resolveUiMode(const QStringList& args, const AppConfig& config) {
    for (const QString& arg : args) {
        if (arg == "--research" || arg == "--ui=research") return UiMode::Research;
        if (arg == "--clinical" || arg == "--ui=clinical") return UiMode::Clinical;
    }
    return parseUiMode(config.uiMode, UiMode::Clinical);
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetDllDirectoryA(nullptr);
#endif

    QApplication app(argc, argv);
    app.setApplicationName("York Clinical Tracking Suite");
    app.setOrganizationName("YCTS");
    app.setWindowIcon(QIcon(":/ui/app_icon.png"));

    AppController controller;
    controller.init();

    const UiMode mode = resolveUiMode(app.arguments(), controller.config());
    DesignSystem::apply(app, mode == UiMode::Clinical ? DesignSystem::Theme::Clinical
                                                      : DesignSystem::Theme::ClinicalSlate);

    MainWindow window(&controller, mode);
    window.show();

    // First launch (no config.ini) or invalid directories: ask for the
    // recordings/protocols folders before the user starts working
    if (controller.configSetupNeeded()) {
        ConfigSetupDialog setup(&controller, &window);
        setup.exec();
    }

    int result = app.exec();
    controller.shutdown();
    return result;
}
