#include "kinect_camera.h"
#include <iostream>

#ifdef HAS_K4A_SDK

KinectCamera::KinectCamera() {}
KinectCamera::~KinectCamera() { close(); }

bool KinectCamera::open(const CameraConfig& config) {
    if (k4a_device_open(config.deviceIndex, &device_) != K4A_RESULT_SUCCEEDED) {
        std::cerr << "Azure Kinect: failed to open device" << std::endl;
        return false;
    }

    k4a_device_configuration_t devConfig = K4A_DEVICE_CONFIG_INIT_DISABLE_ALL;
    devConfig.color_format = K4A_IMAGE_FORMAT_COLOR_BGRA32;
    devConfig.synchronized_images_only = true;

    // Color resolution
    switch (config.k4aColorRes) {
        case K4AColorRes::RES_720P:  devConfig.color_resolution = K4A_COLOR_RESOLUTION_720P; break;
        case K4AColorRes::RES_1080P: devConfig.color_resolution = K4A_COLOR_RESOLUTION_1080P; break;
        case K4AColorRes::RES_1440P: devConfig.color_resolution = K4A_COLOR_RESOLUTION_1440P; break;
        case K4AColorRes::RES_1536P: devConfig.color_resolution = K4A_COLOR_RESOLUTION_1536P; break;
        case K4AColorRes::RES_2160P: devConfig.color_resolution = K4A_COLOR_RESOLUTION_2160P; break;
    }

    // Depth mode
    switch (config.k4aDepthMode) {
        case K4ADepthMode::NFOV_2X2BINNED: devConfig.depth_mode = K4A_DEPTH_MODE_NFOV_2X2BINNED; break;
        case K4ADepthMode::NFOV_UNBINNED:  devConfig.depth_mode = K4A_DEPTH_MODE_NFOV_UNBINNED; break;
        case K4ADepthMode::WFOV_2X2BINNED: devConfig.depth_mode = K4A_DEPTH_MODE_WFOV_2X2BINNED; break;
        case K4ADepthMode::WFOV_UNBINNED:  devConfig.depth_mode = K4A_DEPTH_MODE_WFOV_UNBINNED; break;
    }

    // FPS
    switch (config.k4aFps) {
        case K4AFPS::FPS_5:  devConfig.camera_fps = K4A_FRAMES_PER_SECOND_5; break;
        case K4AFPS::FPS_15: devConfig.camera_fps = K4A_FRAMES_PER_SECOND_15; break;
        case K4AFPS::FPS_30: devConfig.camera_fps = K4A_FRAMES_PER_SECOND_30; break;
    }

    if (k4a_device_start_cameras(device_, &devConfig) != K4A_RESULT_SUCCEEDED) {
        std::cerr << "Azure Kinect: failed to start cameras" << std::endl;
        k4a_device_close(device_);
        device_ = nullptr;
        return false;
    }

    if (k4a_device_get_calibration(device_, devConfig.depth_mode, devConfig.color_resolution, &calibration_) != K4A_RESULT_SUCCEEDED) {
        std::cerr << "Azure Kinect: failed to get calibration" << std::endl;
        k4a_device_stop_cameras(device_);
        k4a_device_close(device_);
        device_ = nullptr;
        return false;
    }

    k4abt_tracker_configuration_t tracker_config = K4ABT_TRACKER_CONFIG_DEFAULT;

    // Processing mode
    switch (config.k4aProcessingMode) {
        case K4AProcessingMode::GPU:
            tracker_config.processing_mode = K4ABT_TRACKER_PROCESSING_MODE_GPU; break;
        case K4AProcessingMode::CPU:
            tracker_config.processing_mode = K4ABT_TRACKER_PROCESSING_MODE_CPU; break;
        case K4AProcessingMode::GPU_CUDA:
            tracker_config.processing_mode = K4ABT_TRACKER_PROCESSING_MODE_GPU_CUDA; break;
        case K4AProcessingMode::GPU_TENSORRT:
            tracker_config.processing_mode = K4ABT_TRACKER_PROCESSING_MODE_GPU_TENSORRT; break;
        case K4AProcessingMode::GPU_DIRECTML:
            tracker_config.processing_mode = K4ABT_TRACKER_PROCESSING_MODE_GPU_DIRECTML; break;
    }

    // Sensor orientation
    switch (config.k4aSensorOrientation) {
        case K4ASensorOrientation::DEFAULT:
            tracker_config.sensor_orientation = K4ABT_SENSOR_ORIENTATION_DEFAULT; break;
        case K4ASensorOrientation::CLOCKWISE90:
            tracker_config.sensor_orientation = K4ABT_SENSOR_ORIENTATION_CLOCKWISE90; break;
        case K4ASensorOrientation::COUNTERCLOCKWISE90:
            tracker_config.sensor_orientation = K4ABT_SENSOR_ORIENTATION_COUNTERCLOCKWISE90; break;
        case K4ASensorOrientation::FLIP180:
            tracker_config.sensor_orientation = K4ABT_SENSOR_ORIENTATION_FLIP180; break;
    }

    if (k4abt_tracker_create(&calibration_, tracker_config, &tracker_) != K4A_RESULT_SUCCEEDED) {
        std::cerr << "Azure Kinect: failed to create body tracker" << std::endl;
        k4a_device_stop_cameras(device_);
        k4a_device_close(device_);
        device_ = nullptr;
        return false;
    }

    opened_ = true;
    return true;
}

void KinectCamera::close() {
    if (opened_) {
        if (tracker_) {
            k4abt_tracker_shutdown(tracker_);
            k4abt_tracker_destroy(tracker_);
            tracker_ = nullptr;
        }
        if (device_) {
            k4a_device_stop_cameras(device_);
            k4a_device_close(device_);
            device_ = nullptr;
        }
        opened_ = false;
    }
}

bool KinectCamera::isOpen() const { return opened_; }

static float confidenceToFloat(k4abt_joint_confidence_level_t level) {
    switch (level) {
        case K4ABT_JOINT_CONFIDENCE_NONE:   return 0.0f;
        case K4ABT_JOINT_CONFIDENCE_LOW:    return 0.33f;
        case K4ABT_JOINT_CONFIDENCE_MEDIUM: return 0.66f;
        case K4ABT_JOINT_CONFIDENCE_HIGH:   return 1.0f;
        default: return 0.0f;
    }
}

bool KinectCamera::grabFrame(FrameData& out) {
    if (!opened_) return false;

    k4a_capture_t capture = nullptr;
    auto result = k4a_device_get_capture(device_, &capture, 1000);
    if (result != K4A_WAIT_RESULT_SUCCEEDED) return false;

    // Get color image
    k4a_image_t colorImage = k4a_capture_get_color_image(capture);
    if (colorImage) {
        int w = k4a_image_get_width_pixels(colorImage);
        int h = k4a_image_get_height_pixels(colorImage);
        out.imageWidth = w;
        out.imageHeight = h;
        uint8_t* buf = k4a_image_get_buffer(colorImage);
        size_t pixelCount = (size_t)w * h;
        size_t dataSize = pixelCount * 4;
        out.imageRGBA.resize(dataSize);
        memcpy(out.imageRGBA.data(), buf, dataSize); // keep native BGRA — GL_BGRA used in renderer
        k4a_image_release(colorImage);
    }

    // Enqueue for body tracking (blocking — recommended by Microsoft samples)
    auto enqResult = k4abt_tracker_enqueue_capture(tracker_, capture, K4A_WAIT_INFINITE);
    k4a_capture_release(capture);

    if (enqResult != K4A_WAIT_RESULT_SUCCEEDED) return false;

    k4abt_frame_t bodyFrame = nullptr;
    auto popResult = k4abt_tracker_pop_result(tracker_, &bodyFrame, K4A_WAIT_INFINITE);
    if (popResult != K4A_WAIT_RESULT_SUCCEEDED) return false;

    out.timestamp_us = k4abt_frame_get_device_timestamp_usec(bodyFrame);
    out.activeJointCount = JOINT_COUNT; // K4ABT always uses 32 joints
    out.bodies.clear();

    uint32_t numBodies = k4abt_frame_get_num_bodies(bodyFrame);
    for (uint32_t i = 0; i < numBodies; ++i) {
        k4abt_skeleton_t skeleton;
        if (k4abt_frame_get_body_skeleton(bodyFrame, i, &skeleton) != K4A_RESULT_SUCCEEDED)
            continue;

        TrackedBody tb;
        tb.id = k4abt_frame_get_body_id(bodyFrame, i);

        for (int j = 0; j < JOINT_COUNT; ++j) {
            auto& kj = skeleton.joints[j];
            // Azure Kinect positions are in millimeters, convert to meters
            // Negate Y because Kinect Y points down from sensor
            tb.joints[j].x = kj.position.xyz.x / 1000.0f;
            tb.joints[j].y = -kj.position.xyz.y / 1000.0f;
            tb.joints[j].z = kj.position.xyz.z / 1000.0f;
            tb.joints[j].confidence = confidenceToFloat(kj.confidence_level);

            // Project 3D joint to 2D color image space
            if (tb.joints[j].confidence >= 0.1f) {
                k4a_float2_t target_point;
                int valid = 0;
                k4a_calibration_3d_to_2d(&calibration_,
                                          &kj.position,
                                          K4A_CALIBRATION_TYPE_DEPTH,
                                          K4A_CALIBRATION_TYPE_COLOR,
                                          &target_point, &valid);
                if (valid) {
                    tb.joints2D[j].x = target_point.xy.x;
                    tb.joints2D[j].y = target_point.xy.y;
                    tb.joints2D[j].valid = true;
                }
            }
        }
        out.bodies.push_back(tb);
    }

    k4abt_frame_release(bodyFrame);
    return true;
}

#else

KinectCamera::KinectCamera() {}
KinectCamera::~KinectCamera() { close(); }
bool KinectCamera::open(const CameraConfig&) {
    std::cerr << "Azure Kinect SDK not available in this build" << std::endl;
    return false;
}
void KinectCamera::close() { opened_ = false; }
bool KinectCamera::isOpen() const { return false; }
bool KinectCamera::grabFrame(FrameData&) { return false; }

#endif
