#include "zed_camera.h"
#include <iostream>
#include <algorithm>
#include <cmath>

#ifdef HAS_ZED_SDK

// Map a ZED keypoint index to this app's canonical 32-joint (Azure Kinect)
// index. Returns -1 to drop the keypoint.
//
// BODY_34's first 32 keypoints already follow the canonical ordering, so it's
// an identity copy (indices 32/33 are heels, which the canonical layout has
// no slot for and are dropped). BODY_18 uses the OpenPose/COCO-18 ordering,
// which is completely different and must be remapped explicitly — without
// this, every BODY_18 joint lands in the wrong slot and bones connect the
// wrong points.
static int zedToCanonical(ZedBodyFormat fmt, int z) {
    if (fmt == ZedBodyFormat::BODY_18) {
        // ZED BODY_18 (OpenPose-18) -> canonical index
        static const int M[18] = {
            27, // 0  Nose          -> NOSE
            3,  // 1  Neck          -> NECK
            12, // 2  Right shoulder-> SHOULDER_RIGHT
            13, // 3  Right elbow   -> ELBOW_RIGHT
            14, // 4  Right wrist   -> WRIST_RIGHT
            5,  // 5  Left shoulder -> SHOULDER_LEFT
            6,  // 6  Left elbow    -> ELBOW_LEFT
            7,  // 7  Left wrist    -> WRIST_LEFT
            22, // 8  Right hip     -> HIP_RIGHT
            23, // 9  Right knee    -> KNEE_RIGHT
            24, // 10 Right ankle   -> ANKLE_RIGHT
            18, // 11 Left hip      -> HIP_LEFT
            19, // 12 Left knee     -> KNEE_LEFT
            20, // 13 Left ankle    -> ANKLE_LEFT
            30, // 14 Right eye     -> EYE_RIGHT
            28, // 15 Left eye      -> EYE_LEFT
            31, // 16 Right ear     -> EAR_RIGHT
            29, // 17 Left ear      -> EAR_LEFT
        };
        return (z >= 0 && z < 18) ? M[z] : -1;
    }
    // BODY_34 / BODY_38: first 32 already match the canonical ordering
    return (z < JOINT_COUNT) ? z : -1;
}

ZedCamera::ZedCamera() {}
ZedCamera::~ZedCamera() { close(); }

bool ZedCamera::open(const CameraConfig& config) {
    sl::InitParameters init;

    // Resolution
    switch (config.zedResolution) {
        case ZedResolution::AUTO:   init.camera_resolution = sl::RESOLUTION::AUTO; break;
        case ZedResolution::HD720:  init.camera_resolution = sl::RESOLUTION::HD720; break;
        case ZedResolution::HD1080: init.camera_resolution = sl::RESOLUTION::HD1080; break;
        case ZedResolution::HD1200: init.camera_resolution = sl::RESOLUTION::HD1200; break;
        case ZedResolution::HD1536: init.camera_resolution = sl::RESOLUTION::HD1536; break;
        case ZedResolution::HD2K:   init.camera_resolution = sl::RESOLUTION::HD2K; break;
    }

    // Depth mode
    switch (config.zedDepthMode) {
        case ZedDepthMode::NEURAL_LIGHT: init.depth_mode = sl::DEPTH_MODE::NEURAL_LIGHT; break;
        case ZedDepthMode::NEURAL:       init.depth_mode = sl::DEPTH_MODE::NEURAL; break;
        case ZedDepthMode::NEURAL_PLUS:  init.depth_mode = sl::DEPTH_MODE::NEURAL_PLUS; break;
    }

    init.coordinate_system = sl::COORDINATE_SYSTEM::RIGHT_HANDED_Y_UP;
    init.coordinate_units = sl::UNIT::METER;
    init.sdk_verbose = 0;

    // Multi-camera: set device ID
    if (config.deviceIndex > 0) {
        init.input.setFromCameraID(config.deviceIndex);
    }

    auto err = zed_.open(init);
    if (err != sl::ERROR_CODE::SUCCESS) {
        std::cerr << "ZED open failed: " << sl::toString(err) << std::endl;
        return false;
    }

    // Enable positional tracking (required for body tracking)
    sl::PositionalTrackingParameters tracking;
    err = zed_.enablePositionalTracking(tracking);
    if (err != sl::ERROR_CODE::SUCCESS) {
        std::cerr << "ZED positional tracking failed: " << sl::toString(err) << std::endl;
        zed_.close();
        return false;
    }

    // Body format
    sl::BodyTrackingParameters bt;
    bt.enable_tracking = true;
    bt.enable_body_fitting = true;

    bodyFormat_ = config.zedBodyFormat;
    switch (config.zedBodyFormat) {
        case ZedBodyFormat::BODY_18:
            bt.body_format = sl::BODY_FORMAT::BODY_18;
            activeJointCount_ = 18;
            break;
        case ZedBodyFormat::BODY_34:
            bt.body_format = sl::BODY_FORMAT::BODY_34;
            activeJointCount_ = 34;
            break;
        case ZedBodyFormat::BODY_38:
            bt.body_format = sl::BODY_FORMAT::BODY_38;
            activeJointCount_ = 38;
            break;
    }

    // Tracking model
    switch (config.zedTrackingModel) {
        case ZedTrackingModel::FAST:
            bt.detection_model = sl::BODY_TRACKING_MODEL::HUMAN_BODY_FAST; break;
        case ZedTrackingModel::MEDIUM:
            bt.detection_model = sl::BODY_TRACKING_MODEL::HUMAN_BODY_MEDIUM; break;
        case ZedTrackingModel::ACCURATE:
            bt.detection_model = sl::BODY_TRACKING_MODEL::HUMAN_BODY_ACCURATE; break;
    }

    err = zed_.enableBodyTracking(bt);
    if (err != sl::ERROR_CODE::SUCCESS) {
        std::cerr << "ZED body tracking enable failed: " << sl::toString(err) << std::endl;
        zed_.close();
        return false;
    }

    opened_ = true;
    return true;
}

void ZedCamera::close() {
    if (opened_) {
        zed_.disableBodyTracking();
        zed_.disablePositionalTracking();
        zed_.close();
        opened_ = false;
    }
}

bool ZedCamera::isOpen() const { return opened_; }

bool ZedCamera::grabFrame(FrameData& out) {
    if (!opened_) return false;

    if (zed_.grab() != sl::ERROR_CODE::SUCCESS)
        return false;

    // Retrieve image
    auto imgErr = zed_.retrieveImage(image_, sl::VIEW::LEFT, sl::MEM::CPU);
    if (imgErr == sl::ERROR_CODE::SUCCESS && image_.isInit()) {
        out.imageWidth = image_.getWidth();
        out.imageHeight = image_.getHeight();
        auto* src = image_.getPtr<sl::uchar4>();
        if (src && out.imageWidth > 0 && out.imageHeight > 0 && out.imageWidth < 8192 && out.imageHeight < 8192) {
            size_t dataSize = (size_t)out.imageWidth * out.imageHeight * 4;
            out.imageRGBA.resize(dataSize);
            memcpy(out.imageRGBA.data(), src, dataSize); // keep native BGRA — GL_BGRA used in renderer
        }
    }

    // Retrieve bodies
    sl::BodyTrackingRuntimeParameters btrt;
    btrt.detection_confidence_threshold = 40;
    zed_.retrieveBodies(bodies_, btrt);

    out.timestamp_us = zed_.getTimestamp(sl::TIME_REFERENCE::IMAGE).getMicroseconds();
    out.activeJointCount = std::min(activeJointCount_, JOINT_COUNT);
    out.bodies.clear();

    for (auto& body : bodies_.body_list) {
        if (body.tracking_state != sl::OBJECT_TRACKING_STATE::OK)
            continue;

        TrackedBody tb;
        tb.id = body.id;

        const int kpCount = (int)body.keypoint.size();
        for (int z = 0; z < kpCount; ++z) {
            const int c = zedToCanonical(bodyFormat_, z);
            if (c < 0 || c >= JOINT_COUNT) continue;

            const auto& kp = body.keypoint[z];
            // ZED reports undetected keypoints as NaN — drawing bones to those
            // produces the random flashing/garbage lines, so skip them.
            if (!std::isfinite(kp.x) || !std::isfinite(kp.y) || !std::isfinite(kp.z))
                continue;

            tb.joints[c].x = kp.x;
            tb.joints[c].y = kp.y;
            tb.joints[c].z = kp.z;
            tb.joints[c].confidence = (z < (int)body.keypoint_confidence.size())
                ? body.keypoint_confidence[z] / 100.0f
                : 0.0f;

            if (z < (int)body.keypoint_2d.size()) {
                tb.joints2D[c].x = body.keypoint_2d[z].x;
                tb.joints2D[c].y = body.keypoint_2d[z].y;
                tb.joints2D[c].valid = (tb.joints[c].confidence >= 0.1f);
            }
        }

        // BODY_18 has no pelvis/spine/head joints — derive them as midpoints
        // so the torso renders and biomechanical angles work (same approach
        // as the RGB pose models).
        if (bodyFormat_ == ZedBodyFormat::BODY_18) {
            auto mid = [&](CanonicalJoint a, CanonicalJoint b, CanonicalJoint dst) {
                const Joint& ja = tb.joints[(int)a];
                const Joint& jb = tb.joints[(int)b];
                Joint& jd = tb.joints[(int)dst];
                if (ja.confidence > 0 && jb.confidence > 0 && jd.confidence == 0) {
                    jd = {(ja.x + jb.x) * 0.5f, (ja.y + jb.y) * 0.5f, (ja.z + jb.z) * 0.5f,
                          std::min(ja.confidence, jb.confidence)};
                    const auto& a2 = tb.joints2D[(int)a];
                    const auto& b2 = tb.joints2D[(int)b];
                    if (a2.valid && b2.valid)
                        tb.joints2D[(int)dst] = {(a2.x + b2.x) * 0.5f, (a2.y + b2.y) * 0.5f, true};
                }
            };
            mid(CanonicalJoint::HIP_LEFT, CanonicalJoint::HIP_RIGHT, CanonicalJoint::PELVIS);
            mid(CanonicalJoint::SHOULDER_LEFT, CanonicalJoint::SHOULDER_RIGHT, CanonicalJoint::SPINE_CHEST);
            mid(CanonicalJoint::SPINE_CHEST, CanonicalJoint::PELVIS, CanonicalJoint::SPINE_NAVEL);
            mid(CanonicalJoint::EAR_LEFT, CanonicalJoint::EAR_RIGHT, CanonicalJoint::HEAD);
            // Clavicles link each shoulder to the chest in the bone graph;
            // BODY_18 lacks them, so the arms would otherwise float free.
            mid(CanonicalJoint::SPINE_CHEST, CanonicalJoint::SHOULDER_LEFT, CanonicalJoint::CLAVICLE_LEFT);
            mid(CanonicalJoint::SPINE_CHEST, CanonicalJoint::SHOULDER_RIGHT, CanonicalJoint::CLAVICLE_RIGHT);
        }

        out.bodies.push_back(tb);
    }

    return true;
}

#else

ZedCamera::ZedCamera() {}
ZedCamera::~ZedCamera() { close(); }
bool ZedCamera::open(const CameraConfig&) {
    std::cerr << "ZED SDK not available in this build" << std::endl;
    return false;
}
void ZedCamera::close() { opened_ = false; }
bool ZedCamera::isOpen() const { return false; }
bool ZedCamera::grabFrame(FrameData&) { return false; }

#endif
