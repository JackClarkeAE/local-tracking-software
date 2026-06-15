#pragma once
#include "icamera.h"
#include "onnx_pose_model.h"
#include <memory>
#include <string>
#include <vector>

// A model found in the RGB_Models directory
struct RgbModelInfo {
    std::string name;      // display name from the sidecar manifest
    std::string onnxPath;  // absolute path to the .onnx file
};

// Scan a directory for drop-in models: every *.onnx with a matching
// *.json sidecar (same base name) is a model.
std::vector<RgbModelInfo> listRgbModels(const std::string& rootDir);

// Wraps an RGB camera and runs a pose model on its frames, so a webcam
// plus a drop-in ONNX model behaves like a markerless tracking camera:
// the same FrameData (image + tracked bodies) flows to display, recording
// and analysis. If the model fails to load or infer, tracking degrades to
// video-only rather than stopping the camera.
class ModelTracker : public ICamera {
public:
    explicit ModelTracker(std::unique_ptr<ICamera> inner) : inner_(std::move(inner)) {}

    bool open(const CameraConfig& config) override;
    void close() override { inner_->close(); }
    bool isOpen() const override { return inner_->isOpen(); }
    bool grabFrame(FrameData& out) override;
    std::string cameraName() const override;

    const std::string& modelError() const { return modelError_; }

private:
    std::unique_ptr<ICamera> inner_;
    OnnxPoseModel model_;
    bool modelOk_ = false;
    std::string modelError_;
};
