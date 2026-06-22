cmake_minimum_required(VERSION 3.20)
project(LocalTrackingSoftware_Qt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files are UTF-8 (string literals include non-ASCII characters);
# MSVC assumes the system codepage unless told otherwise
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Qt requires these
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# =============================================================================
# ZED SDK (optional — Windows/Linux only; requires NVIDIA CUDA)
# Override with -DZED_SDK_ROOT=... or the ZED_SDK_ROOT environment variable.
# =============================================================================
set(ZED_SDK_ROOT "" CACHE PATH "ZED SDK root directory")
if(NOT ZED_SDK_ROOT)
    if(DEFINED ENV{ZED_SDK_ROOT})
        set(ZED_SDK_ROOT "$ENV{ZED_SDK_ROOT}")
    elseif(WIN32)
        set(ZED_SDK_ROOT "C:/Program Files (x86)/ZED SDK")
    elseif(UNIX AND NOT APPLE)
        set(ZED_SDK_ROOT "/usr/local/zed")
    endif()
endif()

set(HAS_ZED_SDK FALSE)
if(ZED_SDK_ROOT AND EXISTS "${ZED_SDK_ROOT}/include/sl/Camera.hpp")
    set(ZED_INCLUDE_DIRS "${ZED_SDK_ROOT}/include")
    if(WIN32)
        set(ZED_LIBRARIES "${ZED_SDK_ROOT}/lib/sl_zed64.lib")
    else()
        find_library(ZED_LIBRARIES NAMES sl_zed HINTS "${ZED_SDK_ROOT}/lib")
    endif()
    if(ZED_LIBRARIES)
        set(HAS_ZED_SDK TRUE)
        message(STATUS "ZED SDK found at ${ZED_SDK_ROOT}")
    endif()
endif()
if(NOT HAS_ZED_SDK)
    message(STATUS "ZED SDK not found — ZED camera support disabled")
endif()

# =============================================================================
# CUDA (required by ZED SDK)
# =============================================================================
if(HAS_ZED_SDK)
    find_package(CUDAToolkit QUIET)
    if(CUDAToolkit_FOUND)
        message(STATUS "CUDA ${CUDAToolkit_VERSION} found")
    else()
        message(WARNING "CUDA not found — ZED SDK may fail to link. Set CUDAToolkit_ROOT.")
    endif()
endif()

# =============================================================================
# Azure Kinect SDK (optional — Windows/Linux only)
# Override with -DK4A_ROOT/-DK4ABT_ROOT or environment variables of the same
# names. On Linux the SDK installs into standard system prefixes, which
# find_path/find_library search by default.
# =============================================================================
set(K4A_ROOT "" CACHE PATH "Azure Kinect Sensor SDK root")
set(K4ABT_ROOT "" CACHE PATH "Azure Kinect Body Tracking SDK root")
if(NOT K4A_ROOT)
    if(DEFINED ENV{K4A_ROOT})
        set(K4A_ROOT "$ENV{K4A_ROOT}")
    elseif(WIN32)
        set(K4A_ROOT "C:/Program Files/Azure Kinect SDK v1.4.1")
    endif()
endif()
if(NOT K4ABT_ROOT)
    if(DEFINED ENV{K4ABT_ROOT})
        set(K4ABT_ROOT "$ENV{K4ABT_ROOT}")
    elseif(WIN32)
        set(K4ABT_ROOT "C:/Program Files/Azure Kinect Body Tracking SDK")
    endif()
endif()

set(HAS_K4A_SDK FALSE)
if(NOT APPLE)
    find_path(K4A_INCLUDE_DIR NAMES k4a/k4a.h
        HINTS "${K4A_ROOT}/sdk/include" "${K4A_ROOT}/include")
    find_path(K4ABT_INCLUDE_DIR NAMES k4abt.h
        HINTS "${K4ABT_ROOT}/sdk/include" "${K4ABT_ROOT}/include")
    find_library(K4A_LIBRARY NAMES k4a
        HINTS "${K4A_ROOT}/sdk/windows-desktop/amd64/release/lib" "${K4A_ROOT}/lib")
    find_library(K4ABT_LIBRARY NAMES k4abt
        HINTS "${K4ABT_ROOT}/sdk/windows-desktop/amd64/release/lib" "${K4ABT_ROOT}/lib")
    if(K4A_INCLUDE_DIR AND K4ABT_INCLUDE_DIR AND K4A_LIBRARY AND K4ABT_LIBRARY)
        set(HAS_K4A_SDK TRUE)
        set(K4A_INCLUDE_DIRS "${K4A_INCLUDE_DIR}" "${K4ABT_INCLUDE_DIR}")
        set(K4A_LIBRARIES "${K4A_LIBRARY}" "${K4ABT_LIBRARY}")
        message(STATUS "Azure Kinect SDK found")
    endif()
endif()
if(NOT HAS_K4A_SDK)
    message(STATUS "Azure Kinect SDK not found — Kinect camera support disabled")
endif()

# =============================================================================
# ONNX Runtime (optional — enables drop-in RGB tracking models)
# Provides a CMake package on Homebrew/vcpkg; otherwise set ONNXRUNTIME_ROOT
# to a prebuilt release (https://github.com/microsoft/onnxruntime/releases).
# =============================================================================
set(HAS_ONNX_RUNTIME FALSE)
find_package(onnxruntime QUIET)
if(onnxruntime_FOUND)
    set(HAS_ONNX_RUNTIME TRUE)
    set(ONNXRUNTIME_TARGET onnxruntime::onnxruntime)
    message(STATUS "ONNX Runtime found (package)")
else()
    set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime prebuilt root")
    if(NOT ONNXRUNTIME_ROOT AND DEFINED ENV{ONNXRUNTIME_ROOT})
        set(ONNXRUNTIME_ROOT "$ENV{ONNXRUNTIME_ROOT}")
    endif()
    find_path(ONNXRUNTIME_INCLUDE_DIR NAMES onnxruntime_cxx_api.h
        HINTS "${ONNXRUNTIME_ROOT}/include" "${ONNXRUNTIME_ROOT}/include/onnxruntime")
    find_library(ONNXRUNTIME_LIBRARY NAMES onnxruntime
        HINTS "${ONNXRUNTIME_ROOT}/lib")
    if(ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
        set(HAS_ONNX_RUNTIME TRUE)
        add_library(onnxruntime_imported SHARED IMPORTED)
        set_target_properties(onnxruntime_imported PROPERTIES
            IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
            IMPORTED_IMPLIB "${ONNXRUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}")
        set(ONNXRUNTIME_TARGET onnxruntime_imported)
        message(STATUS "ONNX Runtime found at ${ONNXRUNTIME_LIBRARY}")
    else()
        message(STATUS "ONNX Runtime not found — RGB tracking models disabled")
    endif()
endif()

# =============================================================================
# Qt 6 (required)
# On Windows the historical default install is used as a hint; everywhere else
# rely on CMAKE_PREFIX_PATH / system packages (apt, Homebrew, aqtinstall).
# =============================================================================
if(WIN32 AND EXISTS "C:/Qt/6.11.0/msvc2022_64")
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.11.0/msvc2022_64")
endif()

# 6.6 minimum: camera footage recording needs QVideoFrameInput. Distro
# packages older than this (e.g. Ubuntu 22.04's Qt 6.2) are rejected here —
# install official Qt instead (scripts/install_deps_linux.sh does this).
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Multimedia Pdf PdfWidgets)
message(STATUS "Found Qt ${Qt6_VERSION} at ${Qt6_DIR}")
get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION)
get_filename_component(QT_BIN_DIR "${_qmake_location}" DIRECTORY)

# =============================================================================
# Sources
# =============================================================================
set(APP_SOURCES
    # Entry point + controller
    src/main.cpp
    src/app_controller.cpp
    src/config.cpp

    # UI
    src/ui/main_window.cpp
    src/ui/design_system.cpp
    src/ui/widget_kit.cpp
    src/ui/info_tab.cpp
    src/ui/experimental_tab.cpp
    src/ui/report_tab.cpp
    src/ui/data_tab.cpp
    src/ui/protocol_editor_tab.cpp
    src/ui/live_tab.cpp
    src/ui/playback_tab.cpp
    src/ui/skeleton_view.cpp
    src/ui/popout_window.cpp
    src/ui/camera_feed_window.cpp
    src/ui/camera_feed_view.cpp
    src/ui/config_setup_dialog.cpp
    src/ui/block_game_view.cpp
    src/ui/resources.qrc

    # Backend — camera
    src/camera/icamera.cpp
    src/camera/zed_camera.cpp
    src/camera/kinect_camera.cpp
    src/camera/webcam_camera.cpp
    src/camera/model_tracker.cpp
    src/camera/onnx_pose_model.cpp

    # Backend — recording
    src/recording/joint_recorder.cpp
    src/recording/playback.cpp
    src/recording/protocol.cpp
    src/recording/flag_recorder.cpp
    src/recording/video_recorder.cpp

    # Backend — data
    src/data/session_scanner.cpp
    src/data/data_exporter.cpp

    # Backend — biofeedback
    src/biofeedback/biofeedback_engine.cpp
    src/biofeedback/biofeedback_recorder.cpp

    # Backend - game
    src/game/block_game.cpp
)

set(APP_HEADERS
    src/app_controller.h
    src/config.h
    src/ui/main_window.h
    src/ui/design_system.h
    src/ui/widget_kit.h
    src/ui/info_tab.h
    src/ui/experimental_tab.h
    src/ui/report_tab.h
    src/ui/data_tab.h
    src/ui/protocol_editor_tab.h
    src/ui/live_tab.h
    src/ui/playback_tab.h
    src/ui/skeleton_view.h
    src/ui/popout_window.h
    src/ui/camera_feed_window.h
    src/ui/camera_feed_view.h
    src/ui/config_setup_dialog.h
    src/ui/block_game_view.h
    src/ui/overlay_state.h
    src/camera/camera_types.h
    src/camera/icamera.h
    src/camera/zed_camera.h
    src/camera/kinect_camera.h
    src/camera/webcam_camera.h
    src/camera/model_tracker.h
    src/camera/onnx_pose_model.h
    src/recording/joint_recorder.h
    src/recording/playback.h
    src/recording/protocol.h
    src/recording/flag_recorder.h
    src/recording/video_recorder.h
    src/recording/ring_buffer.h
    src/data/session_scanner.h
    src/data/data_exporter.h
    src/biofeedback/biofeedback_engine.h
    src/biofeedback/biofeedback_recorder.h
    src/game/block_game.h
)

# =============================================================================
# Executable
# =============================================================================
# Platform app-icon assets
set(APP_ICON_SOURCES "")
if(WIN32)
    list(APPEND APP_ICON_SOURCES src/ui/app_icon.rc)  # embeds app_icon.ico in the exe
elseif(APPLE)
    set(MACOSX_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icon.icns")
    set_source_files_properties("${MACOSX_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    list(APPEND APP_ICON_SOURCES "${MACOSX_ICON}")
endif()

add_executable(${PROJECT_NAME} ${APP_SOURCES} ${APP_HEADERS} ${APP_ICON_SOURCES})

set_target_properties(${PROJECT_NAME} PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "Local Tracking Software"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.localtracking.capture"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_ICON_FILE "icon.icns"
    # Custom template adds NSCameraUsageDescription (required for RGB capture)
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/cmake/MacOSXBundleInfo.plist.in"
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

if(HAS_ZED_SDK)
    target_include_directories(${PROJECT_NAME} PRIVATE ${ZED_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ZED_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_ZED_SDK)
    if(CUDAToolkit_FOUND)
        target_link_libraries(${PROJECT_NAME} PRIVATE CUDA::cudart)
    endif()
endif()

if(HAS_K4A_SDK)
    target_include_directories(${PROJECT_NAME} PRIVATE ${K4A_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${K4A_LIBRARIES})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_K4A_SDK)
endif()

if(HAS_ONNX_RUNTIME)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${ONNXRUNTIME_TARGET})
    target_compile_definitions(${PROJECT_NAME} PRIVATE HAS_ONNX_RUNTIME)
endif()

# Apple permission backends are static plugins that must be linked into the
# executable (qt_add_executable would do this automatically); without this,
# QCameraPermission reports Denied without ever prompting the user
if(APPLE AND TARGET Qt6::QDarwinCameraPermissionPlugin)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::QDarwinCameraPermissionPlugin)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Multimedia
    Qt6::Pdf
    Qt6::PdfWidgets
)

# Windows SDK for file dialogs / shell
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE opengl32 user32 gdi32 shell32)
elseif(UNIX AND NOT APPLE)
    # dlopen used for runtime camera SDK detection
    target_link_libraries(${PROJECT_NAME} PRIVATE dl)
endif()

# =============================================================================
# Tests
# =============================================================================
enable_testing()

add_executable(qt_parity_tests
    tests/qt_parity_tests.cpp
    src/app_controller.cpp
    src/config.cpp
    src/camera/model_tracker.cpp
    src/camera/onnx_pose_model.cpp
    src/recording/joint_recorder.cpp
    src/recording/playback.cpp
    src/recording/protocol.cpp
    src/recording/flag_recorder.cpp
    src/recording/video_recorder.cpp
    src/biofeedback/biofeedback_engine.cpp
    src/biofeedback/biofeedback_recorder.cpp
    src/app_controller.h
    src/recording/video_recorder.h
    src/ui/overlay_state.h
)

target_include_directories(qt_parity_tests PRIVATE src)

target_link_libraries(qt_parity_tests PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Multimedia
)

if(WIN32)
    target_link_libraries(qt_parity_tests PRIVATE user32 shell32)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(qt_parity_tests PRIVATE dl)
endif()

add_test(NAME qt_parity_tests COMMAND qt_parity_tests)
set_tests_properties(qt_parity_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)

# =============================================================================
# Local Qt DLL deployment (Windows) for running straight from the build tree
# =============================================================================
if(WIN32)
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${QT_BIN_DIR}")
    if(WINDEPLOYQT_EXECUTABLE)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                --release
                --no-translations
                --no-system-d3d-compiler
                --no-opengl-sw
                "$<TARGET_FILE:${PROJECT_NAME}>"
            COMMENT "Deploying Qt DLLs")
    endif()

    # Copy ONNX Runtime DLL (drop-in RGB tracking models)
    if(HAS_ONNX_RUNTIME AND ONNXRUNTIME_LIBRARY)
        get_filename_component(_ort_libdir "${ONNXRUNTIME_LIBRARY}" DIRECTORY)
        if(EXISTS "${_ort_libdir}/onnxruntime.dll")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_ort_libdir}/onnxruntime.dll"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
        endif()
    endif()

    # Copy ZED DLL
    if(HAS_ZED_SDK AND EXISTS "${ZED_SDK_ROOT}/bin/sl_zed64.dll")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ZED_SDK_ROOT}/bin/sl_zed64.dll"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
    endif()

    # Copy K4A DLLs
    if(HAS_K4A_SDK)
        set(K4A_BIN_DIR "${K4A_ROOT}/sdk/windows-desktop/amd64/release/bin")
        set(K4ABT_BIN_DIR "${K4ABT_ROOT}/sdk/windows-desktop/amd64/release/bin")
        foreach(dll k4a.dll k4arecord.dll depthengine_2_0.dll)
            if(EXISTS "${K4A_BIN_DIR}/${dll}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${K4A_BIN_DIR}/${dll}"
                        "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
            endif()
        endforeach()
        foreach(dll k4abt.dll directml.dll onnxruntime.dll dnn_model_2_0_op11.onnx)
            if(EXISTS "${K4ABT_BIN_DIR}/${dll}")
                add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${K4ABT_BIN_DIR}/${dll}"
                        "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
            endif()
        endforeach()
    endif()
endif()

# =============================================================================
# Install rules — used by `cmake --install` and CPack
# =============================================================================
include(GNUInstallDirs)

# Bundled read-only RGB tracking models (MoveNet) shipped with the app so the
# RGB demo works out of the box. Destination matches getBundledModelsDir().
set(RGB_MODELS_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../RGB_Models/")

if(APPLE)
    install(TARGETS ${PROJECT_NAME} BUNDLE DESTINATION .)
    # Models go inside the bundle BEFORE macdeployqt/codesign so the signature
    # covers them
    install(DIRECTORY "${RGB_MODELS_SRC}"
            DESTINATION "${PROJECT_NAME}.app/Contents/Resources/RGB_Models"
            FILES_MATCHING PATTERN "*.onnx" PATTERN "*.json" PATTERN "*.txt")
    # Bundle Qt frameworks into the .app so the package is self-contained
    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${QT_BIN_DIR}")
    if(MACDEPLOYQT_EXECUTABLE)
        install(CODE "execute_process(COMMAND \"${MACDEPLOYQT_EXECUTABLE}\" \"\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app\" -always-overwrite)")
        # Building on non-APFS volumes litters AppleDouble (._*) files into the
        # bundle, which breaks codesign — scrub them, then re-sign ad hoc
        install(CODE "execute_process(COMMAND find \"\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app\" -name \"._*\" -delete)")
        install(CODE "execute_process(COMMAND codesign --force --deep --sign - \"\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app\")")
    endif()
elseif(WIN32)
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION .)
    if(WINDEPLOYQT_EXECUTABLE)
        install(CODE "execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" --release --no-translations --no-system-d3d-compiler --no-opengl-sw \"\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.exe\")")
    endif()
    # Camera runtime DLLs (when the build machine has the SDKs)
    install(DIRECTORY "$<TARGET_FILE_DIR:${PROJECT_NAME}>/"
            DESTINATION .
            FILES_MATCHING PATTERN "*.dll" PATTERN "*.onnx")
    install(DIRECTORY "${RGB_MODELS_SRC}"
            DESTINATION "RGB_Models"
            FILES_MATCHING PATTERN "*.onnx" PATTERN "*.json" PATTERN "*.txt")
else()
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(DIRECTORY "${RGB_MODELS_SRC}"
            DESTINATION "${CMAKE_INSTALL_BINDIR}/RGB_Models"
            FILES_MATCHING PATTERN "*.onnx" PATTERN "*.json" PATTERN "*.txt")
endif()

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../protocols"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/localtracking
        OPTIONAL)

# =============================================================================
# CPack — produces the per-platform installer / archive
#   Windows: NSIS installer (+ ZIP)   |  macOS: DMG  |  Linux: DEB + TGZ
# =============================================================================
set(CPACK_PACKAGE_NAME "LocalTrackingSoftware")
set(CPACK_PACKAGE_VENDOR "LocalTracking")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Clinical body-tracking capture suite (Qt)")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Local Tracking Software")

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "Local Tracking Software")
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_PACKAGE_EXECUTABLES "${PROJECT_NAME};Local Tracking Software")
    # Start-menu + desktop shortcuts (the installer offers the desktop one)
    set(CPACK_CREATE_DESKTOP_LINKS "${PROJECT_NAME}")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    # Installer / Add-Remove-Programs / shortcut icon
    set(_winico "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/icon.ico")
    set(CPACK_NSIS_MUI_ICON "${_winico}")
    set(CPACK_NSIS_MUI_UNIICON "${_winico}")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "${PROJECT_NAME}.exe")
elseif(APPLE)
    set(CPACK_GENERATOR "DragNDrop")
else()
    set(CPACK_GENERATOR "TGZ;DEB")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "LocalTracking")
    # shlibdeps fails when Qt comes from aqtinstall rather than apt, so
    # declare the runtime dependencies explicitly
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libqt6core6 | libqt6core6t64, libqt6gui6 | libqt6gui6t64, libqt6widgets6 | libqt6widgets6t64, libqt6opengl6 | libqt6opengl6t64, libqt6openglwidgets6 | libqt6openglwidgets6t64, libqt6multimedia6, libgl1")
endif()

include(CPack)

message(STATUS "")
message(STATUS "================ Build configuration ================")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "  Qt:       ${Qt6_VERSION}")
message(STATUS "  ZED SDK:  ${HAS_ZED_SDK}")
message(STATUS "  K4A SDK:  ${HAS_K4A_SDK}")
message(STATUS "  ONNX RT:  ${HAS_ONNX_RUNTIME}")
message(STATUS "=====================================================")
