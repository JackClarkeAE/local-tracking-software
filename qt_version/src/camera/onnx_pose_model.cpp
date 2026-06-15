#include "onnx_pose_model.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QImage>
#include <cmath>
#include <cstring>
#include <map>

#ifdef HAS_ONNX_RUNTIME
#include <onnxruntime_cxx_api.h>
#endif

namespace {

int canonicalJointIndex(const std::string& name) {
    static const std::map<std::string, int> lookup = [] {
        std::map<std::string, int> m;
        for (int i = 0; i < JOINT_COUNT; ++i) m[jointName(i)] = i;
        return m;
    }();
    auto it = lookup.find(name);
    return it != lookup.end() ? it->second : -1;
}

QJsonObject loadManifest(const std::string& onnxPath, std::string* error) {
    const QString jsonPath = QFileInfo(QString::fromStdString(onnxPath))
                                 .path() + "/" +
                             QFileInfo(QString::fromStdString(onnxPath))
                                 .completeBaseName() + ".json";
    QFile f(jsonPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "Missing sidecar manifest: " + jsonPath.toStdString();
        return {};
    }
    QJsonParseError parseErr;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
    if (!doc.isObject()) {
        if (error) *error = "Invalid JSON in " + jsonPath.toStdString() + ": " +
                            parseErr.errorString().toStdString();
        return {};
    }
    return doc.object();
}

} // namespace

std::string rgbModelDisplayName(const std::string& onnxPath) {
    std::string err;
    const QJsonObject manifest = loadManifest(onnxPath, &err);
    const QString name = manifest.value("name").toString();
    if (!name.isEmpty()) return name.toStdString();
    return QFileInfo(QString::fromStdString(onnxPath)).completeBaseName().toStdString();
}

#ifdef HAS_ONNX_RUNTIME

struct OnnxPoseModel::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "lts_pose"};
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;

    std::string inputName, outputName;
    std::vector<int64_t> inputShape;
    ONNXTensorElementDataType inputType = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
    bool nhwc = true;
    int inW = 0, inH = 0;

    enum class Decoder { KeypointsYXC, KeypointsXYC } decoder = Decoder::KeypointsYXC;
    enum class Normalize { None, ZeroOne, NegOneOne, ImageNet } normalize = Normalize::None;
    float minConfidence = 0.2f;
    std::vector<int> keypointToJoint;  // model keypoint index -> canonical joint (-1 = ignore)

    std::vector<float> floatInput;
    std::vector<int32_t> intInput;
};

OnnxPoseModel::OnnxPoseModel() = default;
OnnxPoseModel::~OnnxPoseModel() = default;

bool OnnxPoseModel::runtimeAvailable() { return true; }

bool OnnxPoseModel::load(const std::string& onnxPath) {
    const QJsonObject manifest = loadManifest(onnxPath, &error_);
    if (manifest.isEmpty()) return false;

    name_ = manifest.value("name").toString(
        QFileInfo(QString::fromStdString(onnxPath)).completeBaseName()).toStdString();

    impl_ = std::make_unique<Impl>();

    // Decoder + normalization from the manifest (the ONNX file can't say)
    const QString decoder = manifest.value("decoder").toString("keypoints_yxc");
    if (decoder == "keypoints_yxc") impl_->decoder = Impl::Decoder::KeypointsYXC;
    else if (decoder == "keypoints_xyc") impl_->decoder = Impl::Decoder::KeypointsXYC;
    else { error_ = "Unknown decoder: " + decoder.toStdString(); return false; }

    const QString norm = manifest.value("normalize").toString("none");
    if (norm == "none") impl_->normalize = Impl::Normalize::None;
    else if (norm == "0_1") impl_->normalize = Impl::Normalize::ZeroOne;
    else if (norm == "neg1_1") impl_->normalize = Impl::Normalize::NegOneOne;
    else if (norm == "imagenet") impl_->normalize = Impl::Normalize::ImageNet;
    else { error_ = "Unknown normalize mode: " + norm.toStdString(); return false; }

    impl_->minConfidence = (float)manifest.value("min_confidence").toDouble(0.2);

    const QJsonArray keypoints = manifest.value("keypoints").toArray();
    if (keypoints.isEmpty()) {
        error_ = "Manifest must list \"keypoints\" (model output order, canonical joint names)";
        return false;
    }
    for (const auto& kp : keypoints)
        impl_->keypointToJoint.push_back(canonicalJointIndex(kp.toString().toStdString()));

    // Tensor specs come from the ONNX file itself
    try {
#ifdef _WIN32
        const std::wstring wpath(onnxPath.begin(), onnxPath.end());
        impl_->session = std::make_unique<Ort::Session>(impl_->env, wpath.c_str(),
                                                        Ort::SessionOptions{});
#else
        impl_->session = std::make_unique<Ort::Session>(impl_->env, onnxPath.c_str(),
                                                        Ort::SessionOptions{});
#endif
        auto inName = impl_->session->GetInputNameAllocated(0, impl_->allocator);
        auto outName = impl_->session->GetOutputNameAllocated(0, impl_->allocator);
        impl_->inputName = inName.get();
        impl_->outputName = outName.get();

        // Keep the TypeInfo alive while reading shape info (the shape info
        // object references it)
        Ort::TypeInfo typeInfo = impl_->session->GetInputTypeInfo(0);
        auto tensorInfo = typeInfo.GetTensorTypeAndShapeInfo();
        impl_->inputShape = tensorInfo.GetShape();
        impl_->inputType = tensorInfo.GetElementType();
        for (auto& d : impl_->inputShape)
            if (d < 0) d = 1;  // dynamic batch -> 1

        if (impl_->inputShape.size() != 4) {
            error_ = "Expected a 4-D image input tensor";
            return false;
        }
        // Layout: channels in dim 1 = NCHW, in dim 3 = NHWC
        if (impl_->inputShape[3] == 3) {
            impl_->nhwc = true;
            impl_->inH = (int)impl_->inputShape[1];
            impl_->inW = (int)impl_->inputShape[2];
        } else if (impl_->inputShape[1] == 3) {
            impl_->nhwc = false;
            impl_->inH = (int)impl_->inputShape[2];
            impl_->inW = (int)impl_->inputShape[3];
        } else {
            error_ = "Could not infer NHWC/NCHW layout from input shape";
            return false;
        }
    } catch (const Ort::Exception& e) {
        error_ = std::string("ONNX Runtime: ") + e.what();
        return false;
    }
    return true;
}

bool OnnxPoseModel::infer(const uint8_t* rgba, int width, int height, TrackedBody& body) {
    if (!impl_ || !impl_->session) return false;
    auto& im = *impl_;

    // Resize frame to the network input size
    const QImage src(rgba, width, height, width * 4, QImage::Format_RGBA8888);
    const QImage scaled = src.scaled(im.inW, im.inH, Qt::IgnoreAspectRatio,
                                     Qt::SmoothTransformation)
                              .convertToFormat(QImage::Format_RGB888);

    const size_t pixels = (size_t)im.inW * im.inH;
    const bool isInt = im.inputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 ||
                       im.inputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8;

    auto channelValue = [&](float v, int c) -> float {
        switch (im.normalize) {
            case Impl::Normalize::ZeroOne:   return v / 255.0f;
            case Impl::Normalize::NegOneOne: return v / 127.5f - 1.0f;
            case Impl::Normalize::ImageNet: {
                static const float mean[3] = {0.485f, 0.456f, 0.406f};
                static const float stdv[3] = {0.229f, 0.224f, 0.225f};
                return (v / 255.0f - mean[c]) / stdv[c];
            }
            default: return v;
        }
    };

    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor{nullptr};

    if (isInt && im.inputType == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32) {
        im.intInput.resize(pixels * 3);
        for (int y = 0; y < im.inH; ++y) {
            const uint8_t* line = scaled.constScanLine(y);
            for (int x = 0; x < im.inW; ++x)
                for (int c = 0; c < 3; ++c) {
                    const int32_t v = line[x * 3 + c];
                    if (im.nhwc) im.intInput[((size_t)y * im.inW + x) * 3 + c] = v;
                    else im.intInput[(size_t)c * pixels + (size_t)y * im.inW + x] = v;
                }
        }
        inputTensor = Ort::Value::CreateTensor<int32_t>(
            memInfo, im.intInput.data(), im.intInput.size(),
            im.inputShape.data(), im.inputShape.size());
    } else {
        im.floatInput.resize(pixels * 3);
        for (int y = 0; y < im.inH; ++y) {
            const uint8_t* line = scaled.constScanLine(y);
            for (int x = 0; x < im.inW; ++x)
                for (int c = 0; c < 3; ++c) {
                    const float v = channelValue(line[x * 3 + c], c);
                    if (im.nhwc) im.floatInput[((size_t)y * im.inW + x) * 3 + c] = v;
                    else im.floatInput[(size_t)c * pixels + (size_t)y * im.inW + x] = v;
                }
        }
        inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, im.floatInput.data(), im.floatInput.size(),
            im.inputShape.data(), im.inputShape.size());
    }

    std::vector<float> raw;
    try {
        const char* inNames[] = {im.inputName.c_str()};
        const char* outNames[] = {im.outputName.c_str()};
        auto outputs = im.session->Run(Ort::RunOptions{nullptr},
                                       inNames, &inputTensor, 1, outNames, 1);
        const float* data = outputs[0].GetTensorData<float>();
        const size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        raw.assign(data, data + count);
    } catch (const Ort::Exception& e) {
        error_ = std::string("Inference failed: ") + e.what();
        return false;
    }

    const size_t numKp = impl_->keypointToJoint.size();
    if (raw.size() < numKp * 3) {
        error_ = "Model output smaller than the manifest keypoint list";
        return false;
    }

    body = TrackedBody{};
    body.id = 1;
    bool any = false;
    for (size_t k = 0; k < numKp; ++k) {
        const int joint = im.keypointToJoint[k];
        if (joint < 0) continue;
        const float a = raw[k * 3 + 0], b = raw[k * 3 + 1], score = raw[k * 3 + 2];
        if (score < im.minConfidence) continue;
        const float xn = (im.decoder == Impl::Decoder::KeypointsYXC) ? b : a;
        const float yn = (im.decoder == Impl::Decoder::KeypointsYXC) ? a : b;

        // Image-plane coordinates scaled to a human-ish size for the 3D view
        const float aspect = height > 0 ? (float)width / (float)height : 1.0f;
        body.joints[joint] = {(xn - 0.5f) * 2.0f * aspect, (0.5f - yn) * 2.0f, 0.0f, score};
        body.joints2D[joint] = {xn * width, yn * height, true};
        any = true;
    }
    if (!any) return false;

    // Derive the canonical joints 2D models don't provide, so skeleton
    // rendering and biomechanical angles work out of the box
    auto mid = [&](CanonicalJoint a, CanonicalJoint b, CanonicalJoint dst) {
        const Joint& ja = body.joints[(int)a];
        const Joint& jb = body.joints[(int)b];
        Joint& jd = body.joints[(int)dst];
        if (ja.confidence > 0 && jb.confidence > 0 && jd.confidence == 0) {
            jd = {(ja.x + jb.x) * 0.5f, (ja.y + jb.y) * 0.5f, (ja.z + jb.z) * 0.5f,
                  std::min(ja.confidence, jb.confidence)};
            const auto& a2 = body.joints2D[(int)a];
            const auto& b2 = body.joints2D[(int)b];
            if (a2.valid && b2.valid)
                body.joints2D[(int)dst] = {(a2.x + b2.x) * 0.5f, (a2.y + b2.y) * 0.5f, true};
        }
    };
    mid(CanonicalJoint::HIP_LEFT, CanonicalJoint::HIP_RIGHT, CanonicalJoint::PELVIS);
    mid(CanonicalJoint::SHOULDER_LEFT, CanonicalJoint::SHOULDER_RIGHT, CanonicalJoint::NECK);
    mid(CanonicalJoint::NECK, CanonicalJoint::PELVIS, CanonicalJoint::SPINE_CHEST);
    mid(CanonicalJoint::SPINE_CHEST, CanonicalJoint::PELVIS, CanonicalJoint::SPINE_NAVEL);
    mid(CanonicalJoint::EAR_LEFT, CanonicalJoint::EAR_RIGHT, CanonicalJoint::HEAD);
    return true;
}

#else // !HAS_ONNX_RUNTIME — stubs so the app builds without the dependency

struct OnnxPoseModel::Impl {};
OnnxPoseModel::OnnxPoseModel() = default;
OnnxPoseModel::~OnnxPoseModel() = default;
bool OnnxPoseModel::runtimeAvailable() { return false; }
bool OnnxPoseModel::load(const std::string&) {
    error_ = "This build was made without ONNX Runtime — RGB tracking models are unavailable.";
    return false;
}
bool OnnxPoseModel::infer(const uint8_t*, int, int, TrackedBody&) { return false; }

#endif
