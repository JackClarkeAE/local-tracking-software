#include "../src/app_controller.h"
#include "../src/camera/icamera.h"
#include "../src/recording/playback.h"
#include "../src/recording/protocol.h"

#include <QCoreApplication>
#include <QColor>
#include <QObject>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

std::unique_ptr<ICamera> createCamera(CameraType) {
    return nullptr;
}

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void expectNear(double actual, double expected, double epsilon, const std::string& message) {
    if (std::fabs(actual - expected) > epsilon) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " (actual=" << actual
                  << ", expected=" << expected << ")\n";
    }
}

void writeText(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string joinFloats(const std::vector<float>& values) {
    std::ostringstream out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out << ",";
        out << values[i];
    }
    return out.str();
}

void testProtocolJsonPreservesParameterStrings(const std::filesystem::path& tempDir) {
    Protocol protocol;
    protocol.name = "Parity Protocol";
    protocol.events = {
        {ProtocolEventType::DisplayText, 1.5f, "Hold|bottom|2.5|cyan"},
        {ProtocolEventType::DisplayShape, 2.0f, "triangle"},
        {ProtocolEventType::ClearOverlay, 3.0f, ""},
        {ProtocolEventType::SetBiofeedbackTransform, 4.0f, "LEFT_KNEE_FLEXION,1.00,1.20,2.0,3.0"},
        {ProtocolEventType::ClearBiofeedbackTransform, 5.0f, "ALL"},
        {ProtocolEventType::PlaySound, 6.0f, "chime"},
        {ProtocolEventType::PlaySound, 7.0f, "C:/sounds/custom cue.wav"},
    };

    const auto path = tempDir / "protocol_roundtrip.json";
    expect(protocol.saveJSON(path.string()), "protocol JSON save should succeed");

    Protocol loaded;
    expect(loaded.loadJSON(path.string()), "protocol JSON load should succeed");
    expect(loaded.name == protocol.name, "protocol name should round-trip");
    expect(loaded.events.size() == protocol.events.size(), "protocol event count should round-trip");
    for (size_t i = 0; i < loaded.events.size() && i < protocol.events.size(); ++i) {
        expect(loaded.events[i].type == protocol.events[i].type, "protocol event type should round-trip");
        expect(loaded.events[i].parameter == protocol.events[i].parameter,
               "protocol parameter string should round-trip unchanged");
    }
}

void testControllerOverlayCountdownAndUnload() {
    AppController controller;
    int overlaySignals = 0;
    QObject::connect(&controller, &AppController::overlayChanged,
                     [&overlaySignals]() { ++overlaySignals; });

    controller.executeProtocolAction({ProtocolEventType::DisplayText, "Hold steady|middle|2.0|cyan"});
    expect(controller.overlayState().text == "Hold steady", "DisplayText should set overlay text");
    expect(controller.overlayState().textPosition == TextPosition::Middle,
           "DisplayText should parse middle text position");
    expectNear(controller.overlayState().textScale, 2.0, 0.001, "DisplayText should parse text scale");
    expect(controller.overlayState().textColor == QColor(80, 230, 230),
           "DisplayText should parse cyan text colour");

    controller.executeProtocolAction({ProtocolEventType::DisplayShape, "circle"});
    expect(controller.overlayState().shape == OverlayShape::RedCircle,
           "DisplayShape should set red circle overlay");

    controller.executeProtocolAction({ProtocolEventType::ClearOverlay, ""});
    expect(controller.overlayState().text.empty(), "ClearOverlay should clear overlay text");
    expect(controller.overlayState().shape == OverlayShape::None,
           "ClearOverlay should clear overlay shape");

    controller.executeProtocolAction({ProtocolEventType::Countdown, "5,1.5"});
    expect(controller.countdownActive, "Countdown action should activate countdown");
    expect(controller.countdownRemaining == 5, "Countdown should parse seconds");
    expectNear(controller.goDisplaySeconds, 1.5, 0.001, "Countdown should parse GO display seconds");

    Protocol protocol;
    protocol.name = "Unload Test";
    protocol.events = {{ProtocolEventType::StartRecording, 0.0f, ""}};
    controller.loadProtocol(protocol);
    controller.runProtocol();
    expect(controller.isProtocolLoaded(), "loadProtocol should mark protocol loaded");
    expect(controller.recordJoints, "loadProtocol should enable joint recording when protocol records");

    controller.setOverlayShape(OverlayShape::GreenTriangle);
    controller.unloadProtocol();
    expect(!controller.isProtocolLoaded(), "unloadProtocol should clear loaded flag");
    expect(!controller.recordJoints, "unloadProtocol should clear recordJoints");
    expect(controller.protocolRunner().state() == ProtocolRunnerState::Idle,
           "unloadProtocol should reset protocol runner");
    expect(controller.loadedProtocol().events.empty(), "unloadProtocol should clear loaded protocol events");
    expect(controller.overlayState().shape == OverlayShape::None,
           "unloadProtocol should clear overlay state");
    expect(overlaySignals >= 4, "overlay changes should emit overlayChanged");
}

void testCameraConfigPropagation() {
    AppController controller;

    controller.setCameraType(1, CameraType::ZED2i);
    expect(controller.cameraType(1) == CameraType::ZED2i, "camera type should update per slot");

    CameraConfig config;
    config.deviceIndex = 99;
    config.zedDepthMode = ZedDepthMode::NEURAL_PLUS;
    config.k4aDepthMode = K4ADepthMode::WFOV_UNBINNED;
    config.k4aFps = K4AFPS::FPS_15;
    controller.setCameraConfig(1, config);

    CameraConfig stored = controller.cameraConfig(1);
    expect(stored.deviceIndex == 1, "camera config setter should pin device index to slot");
    expect(stored.zedDepthMode == ZedDepthMode::NEURAL_PLUS,
           "camera config should preserve ZED depth mode");
    expect(stored.k4aDepthMode == K4ADepthMode::WFOV_UNBINNED,
           "camera config should preserve Kinect depth mode");
    expect(stored.k4aFps == K4AFPS::FPS_15, "camera config should preserve Kinect FPS");

    CameraType before = controller.cameraType(1);
    controller.setCameraType(-1, CameraType::AzureKinect);
    expect(controller.cameraType(1) == before, "invalid camera type slot should be ignored");
}

void testPlaybackLoaders(const std::filesystem::path& tempDir) {
    const auto recordedCsv = tempDir / "walk_20260602_101010_cam1.csv";
    writeText(recordedCsv,
              "time_seconds,body_id,joint_index,joint_name,x,y,z,confidence\n"
              "1000000000000,1,0,PELVIS,1,2,3,0.9\n"
              "1000001000000,1,0,PELVIS,2,3,4,0.8\n");
    writeText(tempDir / "walk_20260602_101010_flags.csv",
              "time_seconds,label\n"
              "0.500000,Turn\n");
    writeText(tempDir / "walk_20260602_101010_metadata.json",
              "{\n"
              "  \"patient_id\": \"walk\",\n"
              "  \"operator\": \"tester\",\n"
              "  \"notes\": \"sidecar metadata\",\n"
              "  \"date\": \"2026-06-02 10:10:10\",\n"
              "  \"cameras\": [{ \"slot\": 0, \"name\": \"Azure Kinect\" }],\n"
              "  \"protocol\": \"Protocol A\",\n"
              "  \"biofeedback_active\": true\n"
              "}\n");

    PlaybackManager csv;
    expect(csv.loadCSV(recordedCsv.string()), "recording CSV loader should accept current format");
    expect(csv.frameCount() == 2, "recording CSV loader should group rows into frames");
    expectNear(csv.duration(), 1.0, 0.001, "recording CSV loader should normalize epoch microseconds");
    expect(csv.flags().size() == 1 && csv.flags()[0].label == "Turn",
           "recording CSV loader should load companion flags");
    expect(csv.metadata().loaded, "recording CSV loader should load companion metadata");
    expect(csv.metadata().patientId == "walk", "metadata patient id should load");
    expect(csv.metadata().protocol == "Protocol A", "metadata protocol should load");
    expect(csv.metadata().biofeedbackActive, "metadata biofeedback flag should load");
    expect(csv.metadata().cameraNames.size() == 1 && csv.metadata().cameraNames[0] == "Azure Kinect",
           "metadata camera names should load");

    const auto resampledCsv = tempDir / "resampled.csv";
    writeText(resampledCsv,
              "time_seconds,pelvis_x,pelvis_y,pelvis_z,left_knee_x,left_knee_y,left_knee_z\n"
              "0.0,1,2,3,4,5,6\n"
              "0.5,7,8,9,10,11,12\n");

    PlaybackManager resampled;
    expect(resampled.loadResampledCSV(resampledCsv.string()),
           "resampled CSV loader should accept wide resampled format");
    expect(resampled.frameCount() == 2, "resampled CSV loader should load both rows");
    expectNear(resampled.duration(), 0.5, 0.001, "resampled CSV duration should use time column");
    const PlaybackFrame* resampledFrame = resampled.getFrame(1);
    expect(resampledFrame && !resampledFrame->bodies.empty(), "resampled frame should contain a body");
    if (resampledFrame && !resampledFrame->bodies.empty()) {
        expectNear(resampledFrame->bodies[0].joints[(int)CanonicalJoint::PELVIS].x, 7.0, 0.001,
                   "resampled pelvis x should map to canonical joint");
        expectNear(resampledFrame->bodies[0].joints[(int)CanonicalJoint::KNEE_LEFT].z, 12.0, 0.001,
                   "resampled left knee z should map to canonical joint");
        expectNear(resampledFrame->bodies[0].joints[(int)CanonicalJoint::PELVIS].confidence, 1.0, 0.001,
                   "resampled joints should receive confidence");
    }

    std::vector<float> legacyValues(96, 0.0f);
    legacyValues[20 * 3 + 0] = 1000.0f;
    legacyValues[20 * 3 + 1] = 2000.0f;
    legacyValues[20 * 3 + 2] = 3000.0f;
    const auto legacyJson = tempDir / "legacy.json";
    writeText(legacyJson,
              "{ \"Frames\": [ { \"Timestamp\": \"00:00:01.00\", \"Joint_Positions\": ["
              + joinFloats(legacyValues) + "] } ] }\n");

    PlaybackManager legacy;
    expect(legacy.loadLegacyJSON(legacyJson.string()), "legacy JSON loader should accept old Kinect format");
    expect(legacy.frameCount() == 1, "legacy JSON loader should load the frame");
    const PlaybackFrame* legacyFrame = legacy.getFrame(0);
    expect(legacyFrame && !legacyFrame->bodies.empty(), "legacy frame should contain a body");
    if (legacyFrame && !legacyFrame->bodies.empty()) {
        const auto& pelvis = legacyFrame->bodies[0].joints[(int)CanonicalJoint::PELVIS];
        expectNear(pelvis.x, 1.0, 0.001, "legacy pelvis x should convert mm to metres");
        expectNear(pelvis.y, -2.0, 0.001, "legacy pelvis y should be negated and converted");
        expectNear(pelvis.z, 3.0, 0.001, "legacy pelvis z should convert mm to metres");
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    const auto tempDir = std::filesystem::current_path() / "qt_parity_test_tmp";
    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir);

    testProtocolJsonPreservesParameterStrings(tempDir);
    testControllerOverlayCountdownAndUnload();
    testCameraConfigPropagation();
    testPlaybackLoaders(tempDir);

    std::filesystem::remove_all(tempDir, ec);

    if (g_failures == 0) {
        std::cout << "qt_parity_tests passed\n";
        return 0;
    }

    std::cerr << g_failures << " qt_parity_tests failure(s)\n";
    return 1;
}
