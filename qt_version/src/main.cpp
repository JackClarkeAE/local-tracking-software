#ifdef _WIN32
#include <windows.h>
#endif

#include <QApplication>
#include "ui/main_window.h"
#include "ui/design_system.h"
#include "ui/config_setup_dialog.h"
#include "app_controller.h"

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetDllDirectoryA(nullptr);
#endif

    QApplication app(argc, argv);
    app.setApplicationName("Local Tracking Software (Qt)");
    app.setOrganizationName("LocalTracking");

    DesignSystem::apply(app);

    AppController controller;
    controller.init();

    MainWindow window(&controller);
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
