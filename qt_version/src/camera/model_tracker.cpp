#include "model_tracker.h"

#include <QDir>
#include <QFileInfo>
#include <algorithm>
#include <iostream>

std::vector<RgbModelInfo> listRgbModels(const std::string& rootDir) {
    std::vector<RgbModelInfo> models;
    const QDir dir(QString::fromStdString(rootDir));
    if (!dir.exists()) return models;

    for (const auto& entry : dir.entryInfoList({"*.onnx"}, QDir::Files, QDir::Name)) {
        const QString sidecar = entry.path() + "/" + entry.completeBaseName() + ".json";
        if (!QFileInfo::exists(sidecar)) continue;
        RgbModelInfo info;
        info.onnxPath = entry.absoluteFilePath().toStdString();
        info.name = rgbModelDisplayName(info.onnxPath);
        models.push_back(std::move(info));
    }
    return models;
}

bool ModelTracker::open(const CameraConfig& config) {
    if (!inner_->open(config)) return false;

    modelOk_ = model_.load(config.rgbModelOnnx);
    if (!modelOk_) {
        modelError_ = model_.error();
        std::cerr << "RGB model load failed (" << config.rgbModelOnnx
                  << "): " << modelError_ << " — continuing video-only" << std::endl;
    }
    return true;
}

bool ModelTracker::grabFrame(FrameData& out) {
    if (!inner_->grabFrame(out)) return false;
    if (!modelOk_ || out.imageRGBA.empty()) return true;  // video-only

    TrackedBody body;
    if (model_.infer(out.imageRGBA.data(), out.imageWidth, out.imageHeight, body)) {
        out.bodies.push_back(std::move(body));
    }
    return true;
}

std::string ModelTracker::cameraName() const {
    std::string name = inner_->cameraName();
    if (modelOk_) name += " + " + model_.name();
    return name;
}
