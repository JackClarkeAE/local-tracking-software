#pragma once
#include "icamera.h"
#include "camera_types.h"

#ifdef HAS_ZED_SDK
#include <sl/Camera.hpp>
#endif

class ZedCamera : public ICamera {
public:
    ZedCamera();
    ~ZedCamera() override;

    bool open(const CameraConfig& config) override;
    void close() override;
    bool isOpen() const override;
    bool grabFrame(FrameData& out) override;
    std::string cameraName() const override { return "ZED2i"; }

private:
    bool opened_ = false;
    int activeJointCount_ = 34;
    ZedBodyFormat bodyFormat_ = ZedBodyFormat::BODY_34;
#ifdef HAS_ZED_SDK
    sl::Camera zed_;
    sl::Bodies bodies_;
    sl::Mat image_;
#endif
};
