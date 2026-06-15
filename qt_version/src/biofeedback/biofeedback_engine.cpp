#include "biofeedback_engine.h"
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================
// Angle Definitions — Lower Limb Set
// ============================================================

static const AngleDefinition s_angleDefinitions[] = {
    { BiomechAngle::LEFT_KNEE_FLEXION,
      "LEFT_KNEE_FLEXION", "Left Knee Flexion",
      18, 19, 20,  // HIP_LEFT, KNEE_LEFT, ANKLE_LEFT
      {20, 21}     // distal chain: ANKLE_LEFT, FOOT_LEFT
    },
    { BiomechAngle::RIGHT_KNEE_FLEXION,
      "RIGHT_KNEE_FLEXION", "Right Knee Flexion",
      22, 23, 24,  // HIP_RIGHT, KNEE_RIGHT, ANKLE_RIGHT
      {24, 25}     // distal chain: ANKLE_RIGHT, FOOT_RIGHT
    },
    { BiomechAngle::LEFT_HIP_FLEXION,
      "LEFT_HIP_FLEXION", "Left Hip Flexion",
      1, 18, 19,   // SPINE_NAVEL, HIP_LEFT, KNEE_LEFT
      {19, 20, 21} // distal chain: KNEE_LEFT, ANKLE_LEFT, FOOT_LEFT
    },
    { BiomechAngle::RIGHT_HIP_FLEXION,
      "RIGHT_HIP_FLEXION", "Right Hip Flexion",
      1, 22, 23,   // SPINE_NAVEL, HIP_RIGHT, KNEE_RIGHT
      {23, 24, 25} // distal chain: KNEE_RIGHT, ANKLE_RIGHT, FOOT_RIGHT
    },
    { BiomechAngle::LEFT_ANKLE_DORSIFLEXION,
      "LEFT_ANKLE_DORSIFLEXION", "Left Ankle Dorsiflexion",
      19, 20, 21,  // KNEE_LEFT, ANKLE_LEFT, FOOT_LEFT
      {21}         // distal chain: FOOT_LEFT
    },
    { BiomechAngle::RIGHT_ANKLE_DORSIFLEXION,
      "RIGHT_ANKLE_DORSIFLEXION", "Right Ankle Dorsiflexion",
      23, 24, 25,  // KNEE_RIGHT, ANKLE_RIGHT, FOOT_RIGHT
      {25}         // distal chain: FOOT_RIGHT
    },
};

static_assert(sizeof(s_angleDefinitions) / sizeof(s_angleDefinitions[0]) == (int)BiomechAngle::COUNT,
              "Angle definitions must match BiomechAngle::COUNT");

const AngleDefinition& getAngleDefinition(BiomechAngle angle) {
    return s_angleDefinitions[(int)angle];
}

const char* biomechAngleName(int index) {
    if (index >= 0 && index < (int)BiomechAngle::COUNT)
        return s_angleDefinitions[index].displayName;
    return "Unknown";
}

int biomechAngleCount() {
    return (int)BiomechAngle::COUNT;
}

int biomechAngleFromName(const std::string& name) {
    for (int i = 0; i < (int)BiomechAngle::COUNT; i++) {
        if (name == s_angleDefinitions[i].name)
            return i;
    }
    return -1;
}

// ============================================================
// BiofeedbackEngine
// ============================================================

void BiofeedbackEngine::clearTransforms() {
    for (auto& t : transforms_) t.active = false;
}

void BiofeedbackEngine::setTransform(BiomechAngle angle, float startScale, float endScale,
                                      float rampStartTime, float rampDuration) {
    auto& t = transforms_[(int)angle];
    t.angle = angle;
    t.startScale = startScale;
    t.endScale = endScale;
    t.rampStartTime = rampStartTime;
    t.rampDuration = rampDuration;
    t.active = true;
}

void BiofeedbackEngine::removeTransform(BiomechAngle angle) {
    transforms_[(int)angle].active = false;
}

bool BiofeedbackEngine::hasActiveTransforms() const {
    for (auto& t : transforms_)
        if (t.active) return true;
    return false;
}

float BiofeedbackEngine::getScaleAtTime(const BiofeedbackTransform& t, float protocolTime) const {
    if (!t.active) return 1.0f;
    if (t.rampDuration <= 0.0f) return t.endScale;

    float elapsed = protocolTime - t.rampStartTime;
    if (elapsed <= 0.0f) return t.startScale;
    if (elapsed >= t.rampDuration) return t.endScale;

    float progress = elapsed / t.rampDuration;
    return t.startScale + (t.endScale - t.startScale) * progress;
}

float BiofeedbackEngine::computeAngle(const Joint& proximal, const Joint& vertex, const Joint& distal) {
    // Vector from vertex to proximal
    float v1x = proximal.x - vertex.x;
    float v1y = proximal.y - vertex.y;
    float v1z = proximal.z - vertex.z;

    // Vector from vertex to distal
    float v2x = distal.x - vertex.x;
    float v2y = distal.y - vertex.y;
    float v2z = distal.z - vertex.z;

    float dot = v1x * v2x + v1y * v2y + v1z * v2z;
    float len1 = std::sqrt(v1x * v1x + v1y * v1y + v1z * v1z);
    float len2 = std::sqrt(v2x * v2x + v2y * v2y + v2z * v2z);

    if (len1 < 1e-6f || len2 < 1e-6f) return 0.0f;

    float cosAngle = dot / (len1 * len2);
    // Clamp to avoid NaN from acos
    if (cosAngle > 1.0f) cosAngle = 1.0f;
    if (cosAngle < -1.0f) cosAngle = -1.0f;

    return std::acos(cosAngle); // radians
}

void BiofeedbackEngine::rotateDistalChain(TrackedBody& body, const AngleDefinition& def,
                                            float angleDeltaRad) {
    if (std::abs(angleDeltaRad) < 1e-6f) return;

    // Get the vertex joint position (rotation center)
    const Joint& vertex = body.joints[def.vertexJoint];

    // Compute rotation axis: cross product of (vertex→proximal) x (vertex→distal)
    const Joint& proximal = body.joints[def.proximalJoint];
    const Joint& distal = body.joints[def.distalJoint];

    float v1x = proximal.x - vertex.x;
    float v1y = proximal.y - vertex.y;
    float v1z = proximal.z - vertex.z;

    float v2x = distal.x - vertex.x;
    float v2y = distal.y - vertex.y;
    float v2z = distal.z - vertex.z;

    // Cross product: v1 × v2
    float ax = v1y * v2z - v1z * v2y;
    float ay = v1z * v2x - v1x * v2z;
    float az = v1x * v2y - v1y * v2x;

    float axLen = std::sqrt(ax * ax + ay * ay + az * az);
    if (axLen < 1e-6f) return; // degenerate — joints are collinear

    // Normalize axis
    ax /= axLen; ay /= axLen; az /= axLen;

    // Rodrigues' rotation formula
    float cosA = std::cos(angleDeltaRad);
    float sinA = std::sin(angleDeltaRad);

    for (int jointIdx : def.distalChain) {
        if (jointIdx >= JOINT_COUNT) continue;
        Joint& j = body.joints[jointIdx];
        if (j.confidence < 0.1f) continue;

        // Translate to vertex-centered coordinates
        float px = j.x - vertex.x;
        float py = j.y - vertex.y;
        float pz = j.z - vertex.z;

        // Rodrigues: p' = p*cos(a) + (axis x p)*sin(a) + axis*(axis . p)*(1-cos(a))
        float dot = ax * px + ay * py + az * pz;
        float crossX = ay * pz - az * py;
        float crossY = az * px - ax * pz;
        float crossZ = ax * py - ay * px;

        float rx = px * cosA + crossX * sinA + ax * dot * (1.0f - cosA);
        float ry = py * cosA + crossY * sinA + ay * dot * (1.0f - cosA);
        float rz = pz * cosA + crossZ * sinA + az * dot * (1.0f - cosA);

        // Translate back
        j.x = rx + vertex.x;
        j.y = ry + vertex.y;
        j.z = rz + vertex.z;

        // Invalidate 2D projections (they're now stale)
        body.joints2D[jointIdx].valid = false;
    }
}

std::vector<AngleMeasurement> BiofeedbackEngine::measureAngles(const FrameData& frame) {
    std::vector<AngleMeasurement> measurements;
    for (int i = 0; i < (int)BiomechAngle::COUNT; i++) {
        const AngleDefinition& def = getAngleDefinition((BiomechAngle)i);
        for (auto& body : frame.bodies) {
            const Joint& proximal = body.joints[def.proximalJoint];
            const Joint& vertex = body.joints[def.vertexJoint];
            const Joint& distal = body.joints[def.distalJoint];
            if (proximal.confidence < 0.1f || vertex.confidence < 0.1f || distal.confidence < 0.1f)
                continue;
            float included = computeAngle(proximal, vertex, distal);
            float flexion = (float)M_PI - included;
            AngleMeasurement m;
            m.angle = (BiomechAngle)i;
            m.rawDegrees = flexion * 180.0f / (float)M_PI;
            m.modifiedDegrees = m.rawDegrees;
            m.scaleFactor = 1.0f;
            measurements.push_back(m);
        }
    }
    return measurements;
}

FrameData BiofeedbackEngine::applyTransforms(const FrameData& rawFrame, float protocolTime) const {
    FrameData modified = rawFrame; // deep copy
    lastMeasurements_.clear();

    for (int i = 0; i < (int)BiomechAngle::COUNT; i++) {
        const auto& t = transforms_[i];
        if (!t.active) continue;

        float scale = getScaleAtTime(t, protocolTime);
        const AngleDefinition& def = getAngleDefinition((BiomechAngle)i);

        for (auto& body : modified.bodies) {
            const Joint& proximal = body.joints[def.proximalJoint];
            const Joint& vertex = body.joints[def.vertexJoint];
            const Joint& distal = body.joints[def.distalJoint];

            if (proximal.confidence < 0.1f || vertex.confidence < 0.1f || distal.confidence < 0.1f)
                continue;

            float includedAngle = computeAngle(proximal, vertex, distal);
            // Flexion = deviation from straight (pi radians)
            float flexion = (float)M_PI - includedAngle;
            float scaledFlexion = flexion * scale;
            float extraFlexion = scaledFlexion - flexion;

            if (std::abs(extraFlexion) > 1e-4f) {
                // Test a small rotation to determine which direction increases flexion
                TrackedBody testBody = body;
                rotateDistalChain(testBody, def, 0.01f);
                float testAngle = computeAngle(testBody.joints[def.proximalJoint],
                                                testBody.joints[def.vertexJoint],
                                                testBody.joints[def.distalJoint]);
                float testFlexion = (float)M_PI - testAngle;
                // If positive rotation increased flexion, keep sign; otherwise negate
                float delta = (testFlexion > flexion) ? extraFlexion : -extraFlexion;
                rotateDistalChain(body, def, delta);
            }

            AngleMeasurement m;
            m.angle = (BiomechAngle)i;
            m.rawDegrees = flexion * 180.0f / (float)M_PI;
            m.modifiedDegrees = scaledFlexion * 180.0f / (float)M_PI;
            m.scaleFactor = scale;
            lastMeasurements_.push_back(m);
        }
    }

    return modified;
}
