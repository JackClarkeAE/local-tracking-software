#pragma once
#include <array>
#include <vector>
#include <string>
#include <cstdint>

constexpr int JOINT_COUNT = 32;

enum class CanonicalJoint : int {
    PELVIS = 0,
    SPINE_NAVEL,
    SPINE_CHEST,
    NECK,
    CLAVICLE_LEFT,
    SHOULDER_LEFT,
    ELBOW_LEFT,
    WRIST_LEFT,
    HAND_LEFT,
    HANDTIP_LEFT,
    THUMB_LEFT,
    CLAVICLE_RIGHT,
    SHOULDER_RIGHT,
    ELBOW_RIGHT,
    WRIST_RIGHT,
    HAND_RIGHT,
    HANDTIP_RIGHT,
    THUMB_RIGHT,
    HIP_LEFT,
    KNEE_LEFT,
    ANKLE_LEFT,
    FOOT_LEFT,
    HIP_RIGHT,
    KNEE_RIGHT,
    ANKLE_RIGHT,
    FOOT_RIGHT,
    HEAD,
    NOSE,
    EYE_LEFT,
    EAR_LEFT,
    EYE_RIGHT,
    EAR_RIGHT
};

struct Joint {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float confidence = 0.0f;
};

struct Joint2D {
    float x = 0.0f;  // pixel x in image space
    float y = 0.0f;  // pixel y in image space
    bool valid = false;
};

struct TrackedBody {
    uint32_t id = 0;
    std::array<Joint, JOINT_COUNT> joints{};
    std::array<Joint2D, JOINT_COUNT> joints2D{};  // image-space positions
};

struct FrameData {
    uint64_t timestamp_us = 0;
    std::vector<TrackedBody> bodies;
    int activeJointCount = JOINT_COUNT; // may be less for BODY_18 etc.
    // Camera image data for background display
    int imageWidth = 0, imageHeight = 0;
    std::vector<uint8_t> imageRGBA; // RGBA pixel data
};

// ============================================================
// Camera configuration
// ============================================================

// ZED-specific options
enum class ZedBodyFormat { BODY_18, BODY_34, BODY_38 };
enum class ZedTrackingModel { FAST, MEDIUM, ACCURATE };
enum class ZedResolution { AUTO, HD720, HD1080, HD1200, HD1536, HD2K };
enum class ZedDepthMode { NEURAL_LIGHT, NEURAL, NEURAL_PLUS };

// Azure Kinect-specific options
enum class K4AProcessingMode { GPU, CPU, GPU_CUDA, GPU_TENSORRT, GPU_DIRECTML };
enum class K4ADepthMode { NFOV_2X2BINNED, NFOV_UNBINNED, WFOV_2X2BINNED, WFOV_UNBINNED };
enum class K4AColorRes { RES_720P, RES_1080P, RES_1440P, RES_1536P, RES_2160P };
enum class K4AFPS { FPS_5, FPS_15, FPS_30 };
enum class K4ASensorOrientation { DEFAULT, CLOCKWISE90, COUNTERCLOCKWISE90, FLIP180 };

struct CameraConfig {
    int deviceIndex = 0; // 0 = first device, 1 = second device

    // RGB webcam: index into the system video-input device list
    int rgbDeviceIndex = 0;
    // Optional drop-in tracking model (path to .onnx; empty = video only)
    std::string rgbModelOnnx;

    // ZED
    ZedBodyFormat zedBodyFormat = ZedBodyFormat::BODY_34;
    ZedTrackingModel zedTrackingModel = ZedTrackingModel::ACCURATE;
    ZedResolution zedResolution = ZedResolution::HD720;
    ZedDepthMode zedDepthMode = ZedDepthMode::NEURAL;

    // Azure Kinect
    K4AProcessingMode k4aProcessingMode = K4AProcessingMode::GPU;
    K4ADepthMode k4aDepthMode = K4ADepthMode::NFOV_UNBINNED;
    K4AColorRes k4aColorRes = K4AColorRes::RES_720P;
    K4AFPS k4aFps = K4AFPS::FPS_30;
    K4ASensorOrientation k4aSensorOrientation = K4ASensorOrientation::DEFAULT;
};

inline const char* jointName(int index) {
    static const char* names[] = {
        "PELVIS", "SPINE_NAVEL", "SPINE_CHEST", "NECK",
        "CLAVICLE_LEFT", "SHOULDER_LEFT", "ELBOW_LEFT", "WRIST_LEFT",
        "HAND_LEFT", "HANDTIP_LEFT", "THUMB_LEFT",
        "CLAVICLE_RIGHT", "SHOULDER_RIGHT", "ELBOW_RIGHT", "WRIST_RIGHT",
        "HAND_RIGHT", "HANDTIP_RIGHT", "THUMB_RIGHT",
        "HIP_LEFT", "KNEE_LEFT", "ANKLE_LEFT", "FOOT_LEFT",
        "HIP_RIGHT", "KNEE_RIGHT", "ANKLE_RIGHT", "FOOT_RIGHT",
        "HEAD", "NOSE", "EYE_LEFT", "EAR_LEFT", "EYE_RIGHT", "EAR_RIGHT"
    };
    if (index >= 0 && index < JOINT_COUNT) return names[index];
    return "UNKNOWN";
}

// Bone connections for skeleton rendering (pairs of joint indices)
inline const std::vector<std::pair<int, int>>& getBoneConnections() {
    static const std::vector<std::pair<int, int>> bones = {
        // Spine
        {0, 1}, {1, 2}, {2, 3}, {3, 26}, // pelvis -> navel -> chest -> neck -> head
        // Left arm
        {2, 4}, {4, 5}, {5, 6}, {6, 7}, {7, 8}, {8, 9}, {7, 10},
        // Right arm
        {2, 11}, {11, 12}, {12, 13}, {13, 14}, {14, 15}, {15, 16}, {14, 17},
        // Left leg
        {0, 18}, {18, 19}, {19, 20}, {20, 21},
        // Right leg
        {0, 22}, {22, 23}, {23, 24}, {24, 25},
        // Face
        {26, 27}, {27, 28}, {27, 30}, {28, 29}, {30, 31},
    };
    return bones;
}
