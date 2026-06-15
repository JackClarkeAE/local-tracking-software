#pragma once
#include "icamera.h"

#ifdef HAS_K4A_SDK
#include <k4a/k4a.h>
#include <k4abt.h>
#endif

class KinectCamera : public ICamera {
public:
    KinectCamera();
    ~KinectCamera() override;

    bool open(const CameraConfig& config) override;
    void close() override;
    bool isOpen() const override;
    bool grabFrame(FrameData& out) override;
    std::string cameraName() const override { return "Azure Kinect"; }

private:
    bool opened_ = false;
#ifdef HAS_K4A_SDK
    k4a_device_t device_ = nullptr;
    k4abt_tracker_t tracker_ = nullptr;
    k4a_calibration_t calibration_{};
#endif
};
