#pragma once
#include "camera_types.h"
#include <string>
#include <vector>
#include <memory>

// Runs a single-stage pose-estimation ONNX model on RGB frames.
//
// A model is a pair of files dropped into the RGB_Models directory:
//   mymodel.onnx   — the network
//   mymodel.json   — sidecar manifest describing what the file can't:
//     {
//       "name":            "MoveNet Lightning",   // shown in the UI
//       "decoder":         "keypoints_yxc",       // output layout (see below)
//       "normalize":       "none",                // none | 0_1 | neg1_1 | imagenet
//       "min_confidence":  0.2,                   // drop keypoints below this
//       "keypoints":       ["NOSE", "EYE_LEFT", ...]  // model output order,
//     }                                           // canonical joint names
//
// Input tensor shape, layout (NHWC/NCHW) and dtype are read from the ONNX
// file itself. Decoders:
//   keypoints_yxc — output [.., K, 3] rows of (y, x, score), coords 0..1
//   keypoints_xyc — output [.., K, 3] rows of (x, y, score), coords 0..1
//
// Missing canonical joints that have an obvious construction (PELVIS = mid
// hips, NECK = mid shoulders, spine points, HEAD) are derived automatically
// so skeleton rendering and angle computation work out of the box.
class OnnxPoseModel {
public:
    OnnxPoseModel();
    ~OnnxPoseModel();

    static bool runtimeAvailable();

    // Loads model + sidecar manifest; returns false with error() set on failure
    bool load(const std::string& onnxPath);
    const std::string& error() const { return error_; }
    const std::string& name() const { return name_; }

    // Run inference on an RGBA frame; fills body (joints + joints2D).
    // Returns false if no keypoints cleared the confidence threshold.
    bool infer(const uint8_t* rgba, int width, int height, TrackedBody& body);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
    std::string name_;
};

// Reads just the "name" field of a sidecar manifest (for UI listings)
std::string rgbModelDisplayName(const std::string& onnxPath);
