#pragma once
// EXPERIMENTAL — anatomical biofeedback engine (development).
//
// Replacement candidate for BiofeedbackEngine's three-point method. Instead of
// rotating a limb about whatever plane its three joints happen to lie in, it
// builds anatomical axes from the pelvis (left hip → right hip never collapses,
// even with a straight leg), measures SIGNED sagittal angles and scales only
// the flexion component about a calibrated neutral:
//
//     target = neutral + gain * (measured - neutral)
//
// Sideways (abduction) and twist components pass through unchanged, so a limb
// can only move further in the chosen direction. Rotations are applied with
// quaternions about the joint's flexion axis, proximal → distal.
//
// Two limb modes:
//   JointAngles — per-joint gains for hip / knee / ankle
//   StepLength  — scales how far the ankle travels in front of / behind the
//                 hip, then solves hip-knee-ankle with two-bone IK (bone
//                 lengths kept, knee keeps pointing forward). Optionally drops
//                 the pelvis when a scaled step is out of reach so the feet
//                 stay planted.
//
// Used only by the Biofeedback Development tab for now; the live patient
// screen still uses BiofeedbackEngine.

#include "../camera/camera_types.h"
#include "bf_math.h"
#include <array>
#include <vector>

namespace anat {

enum class Side { Left = 0, Right = 1 };
enum class LegJoint { Hip = 0, Knee = 1, Ankle = 2 };
enum class LimbMode { JointAngles = 0, StepLength = 1 };
enum class HipReference { Pelvis = 0, WorldVertical = 1 };

struct LegJointIndices { int hip, knee, ankle, foot; };
const LegJointIndices& legJoints(Side side);
const char* sideName(Side side);           // "Left" / "Right"
const char* legJointName(LegJoint joint);  // "Hip" / "Knee" / "Ankle"

// Anatomical pelvis frame. All axes are unit length and mutually orthogonal.
struct BodyAxes {
    bool valid = false;
    bool planar = false;   // 2D (RGB) data: z is always 0, so flexion is measured in the image plane
    bf::Vec3 origin;       // pelvis
    bf::Vec3 ml;           // medio-lateral: toward the subject's right (image normal when planar)
    bf::Vec3 up;           // superior (pelvis → spine, or world vertical — see HipReference)
    bf::Vec3 ant;          // anterior: forwardSign * (ml × up)
    int forwardSign = 1;
};

struct LegMeasure {
    bool valid = false;        // hip, knee and ankle all tracked
    bool ankleValid = false;   // foot also tracked
    float hip = 0.0f;          // radians, + flexion (thigh forward), - extension
    float knee = 0.0f;         // radians, + flexion, 0 = straight, - hyperextension
    float ankle = 0.0f;        // radians, + dorsiflexion, 0 = foot perpendicular to shank
    float stepAP = 0.0f;       // ankle relative to hip joint, metres, + anterior
    float stepML = 0.0f;       // ... + toward the subject's right
    float stepUp = 0.0f;       // ... + superior (negative when standing)
};

struct LimbSettings {
    bool enabled = false;
    LimbMode mode = LimbMode::JointAngles;
    float hipGain = 1.0f;
    float kneeGain = 1.0f;
    float ankleGain = 1.0f;
    float stepGain = 1.0f;
};

struct Neutral {
    float hip = 0.0f, knee = 0.0f, ankle = 0.0f;  // radians
    float stepAP = 0.0f;                          // metres
};

struct Settings {
    std::array<LimbSettings, 2> limbs{};  // indexed by Side
    std::array<Neutral, 2> neutral{};
    HipReference hipReference = HipReference::Pelvis;
    // anterior = forwardSign * (ml × up). Depends on the camera's coordinate
    // handedness (Kinect after its Y flip is left-handed, ZED is right-handed)
    // — use detectForwardSign() on a session rather than assuming.
    int forwardSign = 1;
    bool groundContact = true;   // keep the lowest foot point at its original height
    bool pelvisDrop = true;      // step mode: lower the body when a step is out of reach
    float maxPelvisDrop = 0.15f; // metres
    float minConfidence = 0.1f;
};

struct LimbDiagnostics {
    bool valid = false;
    LegMeasure in, out;
    std::array<float, 3> target{};   // radians — hip, knee, ankle the settings ask for
    float targetStepAP = 0.0f;       // metres
    float lateralShift = 0.0f;       // metres — change of ankle ML offset from the hip (out - in)
    float boneLengthError = 0.0f;    // metres — largest |change| of thigh / shank / foot length
};

struct BodyDiagnostics {
    bool valid = false;
    std::array<LimbDiagnostics, 2> limbs{};
    float groundShift = 0.0f;   // metres applied along world Y by ground contact
    float pelvisDrop = 0.0f;    // metres the body was lowered for step mode
};

BodyAxes computeAxes(const TrackedBody& body, const Settings& settings);
LegMeasure measureLeg(const TrackedBody& body, Side side, const BodyAxes& axes,
                      float minConfidence = 0.1f);

// Votes over the given bodies using the direction the feet point (ankle → foot).
// Returns +1 / -1, or 0 if the feet were never tracked well enough to decide.
int detectForwardSign(const std::vector<const TrackedBody*>& bodies, float minConfidence = 0.1f);

// Neutral pose from a body (e.g. a quiet-standing frame). Limbs that are not
// tracked keep their previous neutral.
void captureNeutral(const TrackedBody& body, Settings& settings);

// Apply the settings. `diag` (optional) receives anatomical measurements of
// input vs output for both legs.
TrackedBody apply(const TrackedBody& in, const Settings& settings, BodyDiagnostics* diag = nullptr);

// Measure any modified body (e.g. the legacy engine's output) against its
// input with the same anatomical definitions, so engines can be compared.
BodyDiagnostics compare(const TrackedBody& in, const TrackedBody& out, const Settings& settings);

} // namespace anat
