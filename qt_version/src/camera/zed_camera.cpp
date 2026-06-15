#include "zed_camera.h"
#include <iostream>
#include <algorithm>

#ifdef HAS_ZED_SDK

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

        int jCount = std::min(activeJointCount_, (int)body.keypoint.size());
        jCount = std::min(jCount, JOINT_COUNT);
        for (int j = 0; j < jCount; ++j) {
            tb.joints[j].x = body.keypoint[j].x;
            tb.joints[j].y = body.keypoint[j].y;
            tb.joints[j].z = body.keypoint[j].z;
            tb.joints[j].confidence = (j < (int)body.keypoint_confidence.size())
                ? body.keypoint_confidence[j] / 100.0f
                : 0.0f;
        }

        // Extract 2D image-space joint positions
        int jCount2d = std::min(jCount, (int)body.keypoint_2d.size());
        for (int j = 0; j < jCount2d; ++j) {
            tb.joints2D[j].x = body.keypoint_2d[j].x;
            tb.joints2D[j].y = body.keypoint_2d[j].y;
            tb.joints2D[j].valid = (tb.joints[j].confidence >= 0.1f);
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
