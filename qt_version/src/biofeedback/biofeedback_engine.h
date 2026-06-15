#pragma once
#include "../camera/camera_types.h"
#include <array>
#include <vector>
#include <string>

// Predefined lower-limb biomechanical angles
enum class BiomechAngle {
    LEFT_KNEE_FLEXION,        // HIP_LEFT → KNEE_LEFT → ANKLE_LEFT
    RIGHT_KNEE_FLEXION,       // HIP_RIGHT → KNEE_RIGHT → ANKLE_RIGHT
    LEFT_HIP_FLEXION,         // SPINE_NAVEL → HIP_LEFT → KNEE_LEFT
    RIGHT_HIP_FLEXION,        // SPINE_NAVEL → HIP_RIGHT → KNEE_RIGHT
    LEFT_ANKLE_DORSIFLEXION,  // KNEE_LEFT → ANKLE_LEFT → FOOT_LEFT
    RIGHT_ANKLE_DORSIFLEXION, // KNEE_RIGHT → ANKLE_RIGHT → FOOT_RIGHT
    COUNT
};

struct AngleDefinition {
    BiomechAngle id;
    const char* name;         // e.g. "LEFT_KNEE_FLEXION"
    const char* displayName;  // e.g. "Left Knee Flexion"
    int proximalJoint;
    int vertexJoint;
    int distalJoint;
    std::vector<int> distalChain; // all joints distal to the vertex that should rotate
};

const AngleDefinition& getAngleDefinition(BiomechAngle angle);
const char* biomechAngleName(int index);
int biomechAngleCount();
int biomechAngleFromName(const std::string& name); // returns -1 if not found

struct BiofeedbackTransform {
    BiomechAngle angle = BiomechAngle::LEFT_KNEE_FLEXION;
    float startScale = 1.0f;
    float endScale = 1.0f;
    float rampStartTime = 0.0f;  // protocol time when ramp begins
    float rampDuration = 0.0f;   // seconds (0 = instant)
    bool active = false;
};

struct AngleMeasurement {
    BiomechAngle angle;
    float rawDegrees = 0.0f;
    float modifiedDegrees = 0.0f;
    float scaleFactor = 1.0f;
};

class BiofeedbackEngine {
public:
    void clearTransforms();
    void setTransform(BiomechAngle angle, float startScale, float endScale,
                      float rampStartTime, float rampDuration);
    void removeTransform(BiomechAngle angle);

    // Apply all active transforms, returns a modified copy
    FrameData applyTransforms(const FrameData& rawFrame, float protocolTime) const;

    const std::vector<AngleMeasurement>& lastMeasurements() const { return lastMeasurements_; }
    bool hasActiveTransforms() const;

    // Get current scale for a transform at a given time
    float getScaleAtTime(const BiofeedbackTransform& t, float protocolTime) const;

    const std::array<BiofeedbackTransform, (int)BiomechAngle::COUNT>& transforms() const { return transforms_; }

    // Measure angles without applying transforms
    static std::vector<AngleMeasurement> measureAngles(const FrameData& frame);
    static float computeAngle(const Joint& proximal, const Joint& vertex, const Joint& distal);

private:
    std::array<BiofeedbackTransform, (int)BiomechAngle::COUNT> transforms_{};
    mutable std::vector<AngleMeasurement> lastMeasurements_;

    static void rotateDistalChain(TrackedBody& body, const AngleDefinition& def, float angleDeltaRad);
};
