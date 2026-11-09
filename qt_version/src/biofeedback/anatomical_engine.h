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
enum class HipReference { Pelvis = 0, WorldVertical = 1 };  // see Settings::hipReference

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

// Physiological limits targets are clamped to (radians). A target is never
// pushed past the measured input, so noisy input outside a range is left as is.
struct JointRange {
    float lo, hi;
};
constexpr float kDegToRad = 3.14159265358979f / 180.0f;
constexpr JointRange kHipRange{-40.0f * kDegToRad, 135.0f * kDegToRad};
constexpr JointRange kKneeRange{-20.0f * kDegToRad, 160.0f * kDegToRad};
constexpr JointRange kAnkleRange{-60.0f * kDegToRad, 45.0f * kDegToRad};

struct Settings {
    std::array<LimbSettings, 2> limbs{};  // indexed by Side
    std::array<Neutral, 2> neutral{};
    // Hip angle measured against the floor's vertical by default: the pelvis
    // "up" axis comes from two joints only ~20 cm apart (pelvis -> navel), so
    // tracking jitter tilts it by degrees per frame (on synthetic gait with
    // 4 mm jitter: 1.8° RMS hip error vs 0.8° for the floor vertical, and
    // 23 mm vs 6 mm step-length error).
    HipReference hipReference = HipReference::WorldVertical;
    // Unit normal of the floor in camera coordinates. Camera Y unless the
    // camera is tilted — see estimateFloor(). Used for ground contact, pelvis
    // drop and the WorldVertical hip reference.
    bf::Vec3 floorUp{0.0f, 1.0f, 0.0f};
    bool limitToJointRanges = true;
    // anterior = forwardSign * (ml × up). Depends on the camera's coordinate
    // handedness (Kinect after its Y flip is left-handed, ZED is right-handed)
    // — use detectForwardSign() on a session rather than assuming.
    int forwardSign = 1;
    bool groundContact = true;   // keep the lowest foot point at its original height
    // With a TemporalState, the vertical corrections (pelvis drop, ground
    // shift) may change by at most this speed between frames, so the avatar
    // can't jump vertically when a foot point drops out of tracking or a
    // near-straight leg makes the required drop spike. 0 = unlimited.
    float maxVerticalSpeed = 0.5f;  // m/s
    bool pelvisDrop = true;      // step mode: lower the body when a step is out of reach
    float maxPelvisDrop = 0.15f; // metres
    // Overground walking (needs a TemporalState): keep the stance foot fixed
    // in the world by moving the whole avatar horizontally. Without it a
    // scaled stance leg slides backward along the floor; with it the avatar
    // travels further than the person, which is what a longer step looks like.
    // Leave off on a treadmill, where the real stance foot moves with the belt.
    bool plantStanceFoot = false;
    float rootRecenterSpeed = 0.25f;  // m/s the avatar drifts back to the person while they stand still
    float minConfidence = 0.1f;
};

struct LimbDiagnostics {
    bool valid = false;
    LegMeasure in, out;
    std::array<float, 3> target{};   // radians — hip, knee, ankle the settings ask for
    float targetStepAP = 0.0f;       // metres
    float lateralShift = 0.0f;       // metres — change of ankle ML offset from the hip (out - in)
    // metres — movement out of each joint's OWN flexion plane, summed down
    // the leg (thigh vs pelvis, shank vs thigh, foot vs shank). A pure
    // flexion change scores 0 even when an abducted thigh tilts the knee's
    // plane and so moves the ankle sideways; the old three-point rotation,
    // which swings limbs out of their planes, does not.
    float offPlane = 0.0f;
    float boneLengthError = 0.0f;    // metres — largest |change| of thigh / shank / foot length
};

struct BodyDiagnostics {
    bool valid = false;
    std::array<LimbDiagnostics, 2> limbs{};
    float groundShift = 0.0f;   // metres applied along the floor normal by ground contact
    float pelvisDrop = 0.0f;    // metres the body was lowered for step mode
    bf::Vec3 rootOffset;        // horizontal shift applied to plant the stance foot
    int stanceSide = -1;        // Side planted this frame, -1 none
};

BodyAxes computeAxes(const TrackedBody& body, const Settings& settings);
LegMeasure measureLeg(const TrackedBody& body, Side side, const BodyAxes& axes,
                      float minConfidence = 0.1f);

// Votes over the given bodies using the direction the feet point (ankle → foot).
// Returns +1 / -1, or 0 if the feet were never tracked well enough to decide.
int detectForwardSign(const std::vector<const TrackedBody*>& bodies, float minConfidence = 0.1f);

// Per-frame forward sign, for data whose facing direction can change during a
// session (2D side view walking back and forth; 3D trackers that swap left and
// right when the subject turns their back). Foot votes are smoothed over
// `window` frames; frames without evidence take the nearest decided sign, and
// `fallback` is used where nothing is known. Null entries are allowed.
std::vector<int> forwardSignsPerFrame(const std::vector<const TrackedBody*>& bodies, int fallback,
                                      int window = 31, float minConfidence = 0.1f);

// Floor plane from the lowest foot point of each frame. When the camera is
// pitched (common for wall/tripod mounts), "up" is not camera Y; using the
// fitted normal keeps ground contact from favouring whichever foot is nearer
// the camera. A tilt can only be measured when the subject travelled >= 1.5 m
// (overground); on a treadmill or standing still camera Y is assumed.
struct FloorPlane {
    bool valid = false;
    bf::Vec3 up{0.0f, 1.0f, 0.0f};
    float height = 0.0f;   // floor offset along `up` (dot(point, up) on the floor)
    float tiltDeg = 0.0f;  // angle between `up` and camera Y
};
FloorPlane estimateFloor(const std::vector<const TrackedBody*>& bodies, float minConfidence = 0.1f);
// Same, from per-frame lowest foot points (see lowestFootPoint)
FloorPlane estimateFloorFromPoints(const std::vector<bf::Vec3>& lowestPoints);
// Lowest tracked ankle / foot point of a body (by camera Y)
bool lowestFootPoint(const TrackedBody& body, bf::Vec3& out, float minConfidence = 0.1f);

// Exchange every left joint with its right counterpart. Body trackers that
// assume the subject faces the camera (Azure Kinect) label the subject's
// right side "left" when they turn their back; this undoes that.
void swapLeftRight(TrackedBody& body);

// Foot-direction evidence for the forward axis in one body: sum over the
// tracked feet of cos(angle between the foot and ml x up); `used` = feet counted.
double forwardVote(const TrackedBody& body, int& used, float minConfidence = 0.1f);

// Neutral pose from a body (e.g. a quiet-standing frame). Limbs that are not
// tracked keep their previous neutral.
void captureNeutral(const TrackedBody& body, Settings& settings);

// State carried between consecutive frames of one body (optional). Pass the
// same object for every frame, in order, with the frame's time; it resets
// itself on a time gap or a different body.
struct TemporalState {
    bool valid = false;
    uint32_t bodyId = 0;
    double time = 0.0;
    float pelvisDrop = 0.0f;
    float groundShift = 0.0f;
    // stance-foot planting
    bf::Vec3 rootOffset;
    bool plantValid = false;
    int stanceSide = -1;
    bf::Vec3 plantPoint;
    bool havePelvis = false;
    bf::Vec3 lastPelvis;
    float stillTime = 0.0f;
    bf::Vec3 lastFoot[2];
    bool lastFootValid[2] = {false, false};
    void reset() { *this = TemporalState{}; }
};

// Apply the settings. `diag` (optional) receives anatomical measurements of
// input vs output for both legs. `state` (optional) enables the temporal
// limits described at Settings::maxVerticalSpeed; `time` is in seconds.
TrackedBody apply(const TrackedBody& in, const Settings& settings, BodyDiagnostics* diag = nullptr,
                  TemporalState* state = nullptr, double time = 0.0);

// Fill short tracking dropouts by linear interpolation between the frames on
// either side (offline: needs the future frame). A dropout that isn't filled
// makes that frame's leg unmeasurable — it snaps back to the unmodified pose —
// and changes which foot point is lowest, so the avatar visibly jumps.
// Filled joints get confidence `filledConfidence`. Returns the number of
// joint samples filled. Null frames (subject absent) break gaps.
int fillTrackingGaps(std::vector<TrackedBody*>& frames, const std::vector<double>& times,
                     double maxGapSeconds = 0.25, float minConfidence = 0.1f,
                     float filledConfidence = 0.34f);

// Measure any modified body (e.g. the legacy engine's output) against its
// input with the same anatomical definitions, so engines can be compared.
BodyDiagnostics compare(const TrackedBody& in, const TrackedBody& out, const Settings& settings);

} // namespace anat
