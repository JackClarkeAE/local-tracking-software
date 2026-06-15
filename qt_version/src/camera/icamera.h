#pragma once
#include "camera_types.h"
#include <memory>
#include <string>

// Camera technology categories. Markerless depth cameras (ZED, Kinect)
// provide skeleton tracking; RGB cameras provide video only; marker-based
// systems are a planned future category.
enum class CameraCategory { Markerless, RGB, Markered };

enum class CameraType { ZED2i, AzureKinect, RGBWebcam };

class ICamera {
public:
    virtual ~ICamera() = default;
    virtual bool open(const CameraConfig& config) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool grabFrame(FrameData& out) = 0;
    virtual std::string cameraName() const = 0;
};

std::unique_ptr<ICamera> createCamera(CameraType type);
