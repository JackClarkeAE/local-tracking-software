#include "icamera.h"
#include "zed_camera.h"
#include "kinect_camera.h"
#include "webcam_camera.h"

std::unique_ptr<ICamera> createCamera(CameraType type) {
    switch (type) {
        case CameraType::ZED2i:       return std::make_unique<ZedCamera>();
        case CameraType::AzureKinect: return std::make_unique<KinectCamera>();
        case CameraType::RGBWebcam:   return std::make_unique<WebcamCamera>();
    }
    return nullptr;
}
