// Unit tests for the experimental anatomical biofeedback engine.
// Pure C++ (no Qt) — builds synthetic legs in both Kinect-style (left-handed)
// and ZED-style (right-handed) coordinates and checks the engine's guarantees:
// signed angles, gains about neutral, abduction preserved, bone lengths kept,
// feet kept on the floor, no NaNs at full extension.

#include "../src/biofeedback/anatomical_engine.h"
#include "../src/biofeedback/anatomical_live.h"
#include "../src/biofeedback/biofeedback_engine.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <vector>
#include <iostream>
#include <random>
#include <string>
#include <tuple>

using bf::Vec3;

namespace {

int g_failures = 0;
constexpr float kDeg = 3.14159265358979f / 180.0f;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << message << "\n";
    }
}

void expectNear(double actual, double expected, double eps, const std::string& message) {
    if (!(std::fabs(actual - expected) <= eps)) {
        ++g_failures;
        std::cerr << "FAIL: " << message << " (actual=" << actual << ", expected=" << expected << ")\n";
    }
}

struct Convention {
    Vec3 ml, up, ant;  // subject's right, superior, anterior — in camera coordinates
    Vec3 origin;
    bool planar = false;
};

// Kinect after the Y flip: x right (camera view), y up, z away from camera.
// Subject faces the camera: anterior = -z, subject's right = -x.
Convention kinectFacingCamera() { return {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0.1f, 0.0f, 2.5f}}; }
// ZED RIGHT_HANDED_Y_UP: x right, y up, z toward the camera. Subject faces camera: anterior = +z.
Convention zedFacingCamera() { return {{-1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.0f, 0.1f, -2.5f}}; }
// RGB side view: 2D image plane, subject walking toward +x, hips overlap.
Convention rgbSideView() {
    Convention c{{0, 0, 1}, {0, 1, 0}, {1, 0, 0}, {0.2f, 0.1f, 0.0f}};
    c.planar = true;
    return c;
}

struct LegPose {
    float hip = 0, knee = 0, ankle = 0, abduction = 0;  // degrees
};

void setJoint(TrackedBody& b, CanonicalJoint j, const Vec3& p) {
    b.joints[(int)j] = {p.x, p.y, p.z, 1.0f};
}

TrackedBody makeBody(const Convention& c, LegPose left, LegPose right) {
    TrackedBody b;
    b.id = 1;
    auto at = [&](float ml, float up, float ant) {
        Vec3 p = c.origin + c.ml * ml + c.up * up + c.ant * ant;
        if (c.planar) p.z = 0.0f;
        return p;
    };
    setJoint(b, CanonicalJoint::PELVIS, at(0, 0, 0));
    setJoint(b, CanonicalJoint::SPINE_NAVEL, at(0, 0.20f, 0));
    setJoint(b, CanonicalJoint::SPINE_CHEST, at(0, 0.40f, 0));
    setJoint(b, CanonicalJoint::NECK, at(0, 0.60f, 0));
    setJoint(b, CanonicalJoint::HEAD, at(0, 0.75f, 0.02f));

    const float hipOffset = c.planar ? 0.0f : 0.09f;
    for (int side = 0; side < 2; ++side) {
        const LegPose& lp = side == 0 ? left : right;
        const float sgn = side == 0 ? -1.0f : 1.0f;  // left hip is toward -ml
        const float th = lp.hip * kDeg, tk = lp.knee * kDeg, ta = lp.ankle * kDeg, ab = lp.abduction * kDeg;
        const Vec3 H = at(sgn * hipOffset, -0.05f, 0);
        // thigh in the sagittal plane, then tilted outward by the abduction
        const Vec3 dSag = c.up * -std::cos(th) + c.ant * std::sin(th);
        const Vec3 d = dSag * std::cos(ab) + c.ml * (sgn * std::sin(ab));
        const Vec3 aT = c.up * std::sin(th) + c.ant * std::cos(th);
        const Vec3 K = H + d * 0.42f;
        const Vec3 s = d * std::cos(tk) - aT * std::sin(tk);
        const Vec3 aS = aT * std::cos(tk) + d * std::sin(tk);
        const Vec3 T = K + s * 0.40f;
        const Vec3 f = aS * std::cos(ta) - s * std::sin(ta);
        const Vec3 F = T + f * 0.15f;
        auto flat = [&](Vec3 p) { if (c.planar) p.z = 0.0f; return p; };
        const auto& L = anat::legJoints((anat::Side)side);
        b.joints[L.hip] = {flat(H).x, flat(H).y, flat(H).z, 1.0f};
        b.joints[L.knee] = {flat(K).x, flat(K).y, flat(K).z, 1.0f};
        b.joints[L.ankle] = {flat(T).x, flat(T).y, flat(T).z, 1.0f};
        b.joints[L.foot] = {flat(F).x, flat(F).y, flat(F).z, 1.0f};
    }
    return b;
}

anat::Settings settingsFor(int forwardSign) {
    anat::Settings s;
    s.forwardSign = forwardSign;
    return s;
}

Vec3 jp(const TrackedBody& b, int j) { return {b.joints[j].x, b.joints[j].y, b.joints[j].z}; }

float boneLen(const TrackedBody& b, int a, int c) { return bf::length(jp(b, c) - jp(b, a)); }

bool allFinite(const TrackedBody& b) {
    for (const auto& j : b.joints)
        if (!std::isfinite(j.x) || !std::isfinite(j.y) || !std::isfinite(j.z)) return false;
    return true;
}

void testForwardSignDetection() {
    const LegPose stand{5, 5, 0, 0};
    for (auto [name, conv, expected] : {std::tuple{"kinect", kinectFacingCamera(), 1},
                                        std::tuple{"zed", zedFacingCamera(), -1},
                                        std::tuple{"rgb side", rgbSideView(), -1}}) {
        const TrackedBody b = makeBody(conv, stand, stand);
        const int sign = anat::detectForwardSign({&b, &b, &b, &b});
        expect(sign == expected, std::string("forward sign detection: ") + name);
    }
}

void testMeasuresSignedAngles() {
    for (auto [name, conv, fwd] : {std::tuple{"kinect", kinectFacingCamera(), 1},
                                   std::tuple{"zed", zedFacingCamera(), -1},
                                   std::tuple{"rgb", rgbSideView(), -1}}) {
        const TrackedBody b = makeBody(conv, {25, 40, 10, 0}, {-12, 5, -8, 0});
        const anat::Settings s = settingsFor(fwd);
        const anat::BodyAxes ax = anat::computeAxes(b, s);
        expect(ax.valid, std::string("axes valid: ") + name);
        expect(ax.planar == conv.planar, std::string("planar detection: ") + name);
        const auto L = anat::measureLeg(b, anat::Side::Left, ax);
        const auto R = anat::measureLeg(b, anat::Side::Right, ax);
        const std::string n = name;
        expectNear(L.hip / kDeg, 25, 0.05, n + " left hip flexion");
        expectNear(L.knee / kDeg, 40, 0.05, n + " left knee flexion");
        expectNear(L.ankle / kDeg, 10, 0.05, n + " left ankle dorsiflexion");
        expectNear(R.hip / kDeg, -12, 0.05, n + " right hip extension is negative");
        expectNear(R.knee / kDeg, 5, 0.05, n + " right knee flexion");
        expectNear(R.ankle / kDeg, -8, 0.05, n + " right ankle plantarflexion is negative");
    }
}

void testUnitGainIsIdentity() {
    const TrackedBody b = makeBody(kinectFacingCamera(), {25, 40, 10, 4}, {-12, 5, -8, 2});
    anat::Settings s = settingsFor(1);
    s.limbs[0].enabled = s.limbs[1].enabled = true;
    s.limbs[1].mode = anat::LimbMode::StepLength;
    const TrackedBody out = anat::apply(b, s);
    double maxErr = 0;
    for (int j = 0; j < JOINT_COUNT; ++j) maxErr = std::max(maxErr, (double)bf::length(jp(out, j) - jp(b, j)));
    expectNear(maxErr, 0.0, 1e-4, "gain 1 leaves the body unchanged");
}

void testHipGainOnlyMovesSagittally() {
    for (auto [name, conv, fwd] : {std::tuple{"kinect", kinectFacingCamera(), 1},
                                   std::tuple{"zed", zedFacingCamera(), -1}}) {
        // thigh abducted 10° — the legacy engine would amplify this sideways too
        const TrackedBody b = makeBody(conv, {20, 30, 5, 10}, {0, 3, 0, 0});
        anat::Settings s = settingsFor(fwd);
        s.groundContact = false;
        s.limbs[0].enabled = true;
        s.limbs[0].hipGain = 1.5f;
        anat::BodyDiagnostics d;
        const TrackedBody out = anat::apply(b, s, &d);
        const std::string n = name;
        expect(d.valid && d.limbs[0].valid, n + " diagnostics valid");
        expectNear(d.limbs[0].out.hip / kDeg, 30, 0.05, n + " hip flexion scaled 20 -> 30");
        expectNear(d.limbs[0].out.knee / kDeg, 30, 0.05, n + " knee flexion untouched by hip gain");
        expectNear(d.limbs[0].lateralShift * 1000, 0, 0.01, n + " ankle does not move sideways");
        const auto& L = anat::legJoints(anat::Side::Left);
        const Vec3 kneeMlIn = jp(b, L.knee) - jp(b, L.hip), kneeMlOut = jp(out, L.knee) - jp(out, L.hip);
        expectNear(bf::dot(kneeMlOut, conv.ml), bf::dot(kneeMlIn, conv.ml), 1e-5, n + " abduction preserved");
        expectNear(d.limbs[0].boneLengthError * 1000, 0, 0.01, n + " bone lengths preserved");
        expectNear(d.limbs[1].out.hip, d.limbs[1].in.hip, 1e-6, n + " other leg untouched");
    }
}

void testExtensionStaysExtension() {
    const TrackedBody b = makeBody(kinectFacingCamera(), {-10, 8, 0, 0}, {0, 0, 0, 0});
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    s.limbs[0].enabled = true;
    s.limbs[0].hipGain = 1.5f;
    anat::BodyDiagnostics d;
    anat::apply(b, s, &d);
    expectNear(d.limbs[0].out.hip / kDeg, -15, 0.05, "hip extension -10 scaled to -15 (sign kept)");
}

void testStraightKneeIsStable() {
    for (float knee : {0.0f, 0.5f, 2.0f}) {
        const TrackedBody b = makeBody(kinectFacingCamera(), {10, knee, 0, 0}, {0, 0, 0, 0});
        anat::Settings s = settingsFor(1);
        s.groundContact = false;
        s.limbs[0].enabled = true;
        s.limbs[0].kneeGain = 2.0f;
        anat::BodyDiagnostics d;
        const TrackedBody out = anat::apply(b, s, &d);
        expect(allFinite(out), "no NaN with a near-straight knee");
        expectNear(d.limbs[0].out.knee / kDeg, 2.0 * knee, 0.05,
                   "knee gain at " + std::to_string(knee) + " deg");
        expectNear(d.limbs[0].lateralShift * 1000, 0, 0.01, "straight knee: no sideways jump");
    }
}

void testNeutralCalibration() {
    const TrackedBody standing = makeBody(kinectFacingCamera(), {6, 4, 3, 0}, {6, 4, 3, 0});
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    anat::captureNeutral(standing, s);
    s.limbs[0].enabled = s.limbs[1].enabled = true;
    s.limbs[0].hipGain = s.limbs[0].kneeGain = s.limbs[0].ankleGain = 2.0f;
    anat::BodyDiagnostics d;
    anat::apply(standing, s, &d);
    expectNear(d.limbs[0].out.hip / kDeg, 6, 0.05, "neutral pose is not exaggerated (hip)");
    expectNear(d.limbs[0].out.knee / kDeg, 4, 0.05, "neutral pose is not exaggerated (knee)");

    const TrackedBody flexed = makeBody(kinectFacingCamera(), {16, 4, 3, 0}, {6, 4, 3, 0});
    anat::apply(flexed, s, &d);
    expectNear(d.limbs[0].out.hip / kDeg, 26, 0.05, "gain applies to the excursion from neutral");
}

void testGroundContact() {
    // Stance leg (right, toes up so the ankle is the lowest point): a knee gain
    // lifts the ankle off the floor; ground contact puts it back
    const TrackedBody b = makeBody(kinectFacingCamera(), {30, 50, 0, 0}, {-5, 10, 25, 0});
    anat::Settings s = settingsFor(1);
    s.limbs[1].enabled = true;
    s.limbs[1].kneeGain = 2.0f;
    auto lowest = [](const TrackedBody& t) {
        float y = 1e9f;
        for (int j : {20, 21, 24, 25}) y = std::min(y, t.joints[j].y);
        return y;
    };
    s.groundContact = false;
    const TrackedBody floating = anat::apply(b, s);
    expect(lowest(floating) - lowest(b) > 0.005f, "without ground contact the stance foot lifts");
    s.groundContact = true;
    anat::BodyDiagnostics d;
    const TrackedBody out = anat::apply(b, s, &d);
    expectNear(lowest(out), lowest(b), 1e-5, "lowest foot point keeps its height");
    expect(d.groundShift < -0.005f, "ground contact shifts the body down");
}

void testStepLengthMode() {
    for (bool drop : {true, false}) {
        // Heel strike: front leg near-straight — a longer step needs the pelvis to drop
        const TrackedBody b = makeBody(kinectFacingCamera(), {25, 4, 0, 0}, {-15, 8, 0, 0});
        anat::Settings s = settingsFor(1);
        s.pelvisDrop = drop;
        for (int side = 0; side < 2; ++side) {
            s.limbs[side].enabled = true;
            s.limbs[side].mode = anat::LimbMode::StepLength;
            s.limbs[side].stepGain = 1.3f;
        }
        anat::BodyDiagnostics d;
        const TrackedBody out = anat::apply(b, s, &d);
        const std::string n = drop ? "step (pelvis drop)" : "step (no drop)";
        expect(allFinite(out), n + ": finite");
        expect(d.limbs[0].valid && d.limbs[1].valid, n + ": diagnostics valid");
        for (int side = 0; side < 2; ++side) {
            const auto& L = anat::legJoints((anat::Side)side);
            expectNear(d.limbs[side].boneLengthError * 1000, 0, 0.05, n + ": bone lengths kept");
            // soft IK may leave a foot at full reach a couple of mm short
            expectNear(out.joints[L.ankle].y, b.joints[L.ankle].y, 0.004, n + ": ankle height kept");
            expectNear(d.limbs[side].lateralShift * 1000, 0, 0.05, n + ": no sideways movement");
        }
        if (drop) {
            expect(d.pelvisDrop > 0.001f, n + ": pelvis lowered");
            expectNear(d.limbs[0].out.stepAP, 1.3 * d.limbs[0].in.stepAP, 0.004, n + ": front step scaled 1.3x");
            expectNear(d.limbs[1].out.stepAP, 1.3 * d.limbs[1].in.stepAP, 0.004, n + ": rear step scaled 1.3x");
        } else {
            expectNear(d.pelvisDrop, 0, 1e-6, n + ": pelvis not lowered");
            // A near-straight leg is already at full reach: without dropping the
            // body the step can't get longer — but it must never get shorter
            expect(d.limbs[0].out.stepAP >= d.limbs[0].in.stepAP - 0.004f, n + ": never (meaningfully) shorter than input");
            expect(d.limbs[1].out.stepAP < d.limbs[1].in.stepAP, n + ": rear step (bent knee) lengthened");
            expect(d.limbs[0].out.stepAP <= 1.3f * d.limbs[0].in.stepAP + 1e-4f, n + ": clamped to reach");
        }
    }
}

void testPlanarDataStaysInPlane() {
    const TrackedBody b = makeBody(rgbSideView(), {20, 30, 0, 0}, {-10, 5, 0, 0});
    anat::Settings s = settingsFor(-1);
    s.limbs[0].enabled = true;
    s.limbs[0].hipGain = 1.5f;
    s.limbs[0].kneeGain = 1.5f;
    anat::BodyDiagnostics d;
    const TrackedBody out = anat::apply(b, s, &d);
    float maxZ = 0;
    for (const auto& j : out.joints) maxZ = std::max(maxZ, std::fabs(j.z));
    expectNear(maxZ, 0, 1e-6, "2D data stays in the image plane");
    expectNear(d.limbs[0].out.hip / kDeg, 30, 0.05, "2D hip gain");
    expectNear(d.limbs[0].out.knee / kDeg, 45, 0.05, "2D knee gain");
}

void testLowConfidenceFootFollowsChain() {
    TrackedBody b = makeBody(kinectFacingCamera(), {20, 30, 5, 0}, {0, 0, 0, 0});
    const auto& L = anat::legJoints(anat::Side::Left);
    b.joints[L.foot].confidence = 0.05f;  // briefly lost
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    s.limbs[0].enabled = true;
    s.limbs[0].kneeGain = 1.5f;
    const TrackedBody out = anat::apply(b, s);
    expectNear(boneLen(out, L.ankle, L.foot), boneLen(b, L.ankle, L.foot), 1e-5,
               "low-confidence foot stays attached to the ankle");
}

// ---------------------------------------------------------------------------
// Edge cases found in review
// ---------------------------------------------------------------------------

void testNonFiniteJointsAreIgnored() {
    TrackedBody b = makeBody(kinectFacingCamera(), {20, 30, 5, 0}, {0, 5, 0, 0});
    const auto& L = anat::legJoints(anat::Side::Left);
    const auto& R = anat::legJoints(anat::Side::Right);
    b.joints[R.knee].x = std::numeric_limits<float>::quiet_NaN();  // confident but NaN
    b.joints[L.foot].y = std::numeric_limits<float>::infinity();
    anat::Settings s = settingsFor(1);
    for (int side = 0; side < 2; ++side) {
        s.limbs[side].enabled = true;
        s.limbs[side].hipGain = s.limbs[side].kneeGain = s.limbs[side].ankleGain = 1.7f;
    }
    anat::BodyDiagnostics d;
    const TrackedBody out = anat::apply(b, s, &d);
    for (int j = 0; j < JOINT_COUNT; ++j) {
        if (j == R.knee || j == L.foot) continue;
        expect(std::isfinite(out.joints[j].x) && std::isfinite(out.joints[j].y) && std::isfinite(out.joints[j].z),
               "NaN/inf in one joint must not spread (joint " + std::to_string(j) + ")");
    }
    expect(!d.limbs[1].in.valid, "leg with a NaN knee is not measured");
    expect(d.limbs[0].in.valid && !d.limbs[0].in.ankleValid, "inf foot: leg measured, ankle skipped");
    expectNear(d.limbs[0].out.hip / kDeg, 34, 0.05, "other joints of that leg still modified");

    // NaN in a hip: no axes, body untouched
    TrackedBody c = makeBody(kinectFacingCamera(), {20, 30, 5, 0}, {0, 5, 0, 0});
    c.joints[R.hip].z = std::numeric_limits<float>::quiet_NaN();
    const TrackedBody outC = anat::apply(c, s);
    for (int j = 0; j < JOINT_COUNT; ++j)
        if (j != R.hip) expectNear(bf::length(jp(outC, j) - jp(c, j)), 0, 0, "NaN hip: body unchanged");
}

void testNonFiniteGainIsIgnored() {
    const TrackedBody b = makeBody(kinectFacingCamera(), {20, 30, 5, 0}, {-5, 5, 0, 0});
    anat::Settings s = settingsFor(1);
    s.limbs[0].enabled = s.limbs[1].enabled = true;
    s.limbs[0].hipGain = std::numeric_limits<float>::quiet_NaN();
    s.limbs[1].mode = anat::LimbMode::StepLength;
    s.limbs[1].stepGain = std::numeric_limits<float>::infinity();
    const TrackedBody out = anat::apply(b, s);
    expect(allFinite(out), "NaN / inf gains never produce NaN joints");
}

void testJointRangeLimits() {
    // Deep knee flexion x2 would fold the shank through the thigh
    const TrackedBody b = makeBody(kinectFacingCamera(), {60, 120, 10, 0}, {0, 5, 0, 0});
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    s.limbs[0].enabled = true;
    s.limbs[0].kneeGain = 2.0f;
    s.limbs[0].hipGain = 3.0f;
    s.limbs[0].ankleGain = 6.0f;
    anat::BodyDiagnostics d;
    anat::apply(b, s, &d);
    expectNear(d.limbs[0].out.knee / kDeg, 160, 0.05, "knee target clamped to 160");
    expectNear(d.limbs[0].out.hip / kDeg, 135, 0.05, "hip target clamped to 135");
    expectNear(d.limbs[0].out.ankle / kDeg, 45, 0.05, "ankle target clamped to 45");
    expectNear(d.limbs[0].target[1] / kDeg, 160, 0.05, "diagnostic target shows the clamp");

    // Input already outside the range is never pulled back by the clamp
    const TrackedBody hyper = makeBody(kinectFacingCamera(), {0, -25, 0, 0}, {0, 5, 0, 0});
    s.limbs[0].hipGain = 1.0f;
    s.limbs[0].kneeGain = 0.5f;  // halving -25 gives -12.5, inside the range
    anat::apply(hyper, s, &d);
    expectNear(d.limbs[0].out.knee / kDeg, -12.5, 0.05, "hyperextension halved");
    s.limbs[0].kneeGain = 1.5f;  // -37.5 is past -20, but the input (-25) already is: clamp to the input
    anat::apply(hyper, s, &d);
    expectNear(d.limbs[0].out.knee / kDeg, -25, 0.05, "clamp never goes past the measured input");

    s.limitToJointRanges = false;
    s.limbs[0].kneeGain = 1.5f;
    anat::apply(hyper, s, &d);
    expectNear(d.limbs[0].out.knee / kDeg, -37.5, 0.05, "limits can be switched off");
}

void testGainZeroFreezesAtNeutral() {
    const TrackedBody b = makeBody(kinectFacingCamera(), {25, 40, 10, 0}, {0, 5, 0, 0});
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    s.neutral[0] = {3 * kDeg, 2 * kDeg, 1 * kDeg, 0.0f};
    s.limbs[0].enabled = true;
    s.limbs[0].hipGain = s.limbs[0].kneeGain = s.limbs[0].ankleGain = 0.0f;
    anat::BodyDiagnostics d;
    anat::apply(b, s, &d);
    expectNear(d.limbs[0].out.hip / kDeg, 3, 0.05, "gain 0 holds hip at neutral");
    expectNear(d.limbs[0].out.knee / kDeg, 2, 0.05, "gain 0 holds knee at neutral");
    expectNear(d.limbs[0].out.ankle / kDeg, 1, 0.05, "gain 0 holds ankle at neutral");
}

void testOneLegStepModeKeepsOtherFootPlanted() {
    const TrackedBody b = makeBody(kinectFacingCamera(), {25, 4, 0, 0}, {-15, 8, 0, 0});
    anat::Settings s = settingsFor(1);
    s.limbs[0].enabled = true;
    s.limbs[0].mode = anat::LimbMode::StepLength;
    s.limbs[0].stepGain = 1.3f;
    anat::BodyDiagnostics d;
    const TrackedBody out = anat::apply(b, s, &d);
    const auto& R = anat::legJoints(anat::Side::Right);
    expect(d.pelvisDrop > 0.001f, "front leg needs the pelvis lowered");
    expectNear(bf::length(jp(out, R.ankle) - jp(b, R.ankle)) * 1000, 0, 4.0, "unmodified leg's ankle stays put");
    expectNear(bf::length(jp(out, R.foot) - jp(b, R.foot)) * 1000, 0, 4.0, "unmodified leg's foot stays put");
    expectNear(d.limbs[1].boneLengthError * 1000, 0, 0.05, "unmodified leg keeps bone lengths");
}

void testTiltedFloor() {
    // Camera pitched 15° down: camera Y is not vertical. Build the body in a
    // frame whose up is tilted, then check the floor fit and ground contact.
    const float t = 15.0f * kDeg;
    Convention c = kinectFacingCamera();
    c.up = Vec3(0, std::cos(t), -std::sin(t));
    c.ant = bf::cross(c.ml, c.up);  // Kinect convention: anterior = ml x up
    std::vector<TrackedBody> frames;
    for (int i = 0; i < 200; ++i) {
        const float ph = i * 0.05f;
        Convention ci = c;
        ci.origin = c.origin + c.ant * (0.6f * ph * 0.5f);  // walking toward the camera
        // feet at floor level: shift so the lowest foot point sits on a fixed plane
        TrackedBody b = makeBody(ci, {20 * std::sin(ph), 20 + 15 * std::sin(ph), 0, 0},
                                 {-20 * std::sin(ph), 20 - 15 * std::sin(ph), 0, 0});
        float low = 1e9f;
        for (int j : {20, 21, 24, 25}) low = std::min(low, bf::dot(jp(b, j), c.up));
        const Vec3 shift = c.up * (-0.85f - low);
        for (auto& jt : b.joints) { jt.x += shift.x; jt.y += shift.y; jt.z += shift.z; }
        frames.push_back(b);
    }
    std::vector<const TrackedBody*> ptrs;
    for (auto& f : frames) ptrs.push_back(&f);
    const anat::FloorPlane fp = anat::estimateFloor(ptrs);
    expect(fp.valid, "floor fitted");
    expectNear(fp.tiltDeg, 15, 1.0, "floor tilt recovered");
    expectNear(bf::dot(fp.up, c.up), 1, 0.003, "floor normal matches true up");

    anat::Settings s = settingsFor(1);
    s.floorUp = fp.up;
    s.limbs[1].enabled = true;
    s.limbs[1].kneeGain = 1.8f;
    for (const auto& b : frames) {
        const TrackedBody out = anat::apply(b, s);
        float lowIn = 1e9f, lowOut = 1e9f;
        for (int j : {20, 21, 24, 25}) {
            lowIn = std::min(lowIn, bf::dot(jp(b, j), c.up));
            lowOut = std::min(lowOut, bf::dot(jp(out, j), c.up));
        }
        if (std::fabs(lowOut - lowIn) > 0.002f) {
            expectNear(lowOut, lowIn, 0.002, "tilted floor: lowest foot keeps its height above the floor");
            break;
        }
    }
}

void testTreadmillFloorIsNotTilted() {
    // On a treadmill the lowest tracked point is the ankle (~7 cm up) at heel
    // strike in front and the toe on the floor at push-off behind: height
    // correlates with front/back position although the floor is level.
    std::vector<TrackedBody> frames;
    for (int i = 0; i < 600; ++i) {
        const float ph = (i % 33) / 33.0f;                 // gait cycle
        const float z = 2.2f + 0.8f * ph;                 // stance foot sweeps back 0.8 m
        const float y = -0.835f + 0.075f * (1.0f - ph);   // heel strike high, push-off on floor
        TrackedBody b = makeBody(kinectFacingCamera(), {5, 5, 0, 0}, {5, 5, 0, 0});
        for (int j : {20, 21, 24, 25}) b.joints[j] = {0.1f, 1.0f, 2.5f, 1.0f};  // lifted out of the way
        b.joints[21] = {0.1f, y, z, (i % 7 == 0) ? 0.0f : 1.0f};  // with some dropouts
        b.joints[20] = {0.1f, y + 0.07f, z, 1.0f};
        frames.push_back(b);
    }
    std::vector<const TrackedBody*> ptrs;
    for (auto& f : frames) ptrs.push_back(&f);
    const anat::FloorPlane fp = anat::estimateFloor(ptrs);
    expect(fp.valid, "treadmill floor valid");
    expectNear(fp.tiltDeg, 0.0, 0.01, "treadmill (no travel): no tilt is invented from heel/toe contact");
}

void testForwardSignsPerFrame() {
    // RGB side view walking right then turning and walking left
    std::vector<TrackedBody> frames;
    for (int i = 0; i < 120; ++i) {
        Convention c = rgbSideView();
        if (i >= 60) c.ant = Vec3(-1, 0, 0);
        const float ph = i * 0.2f;
        frames.push_back(makeBody(c, {15 * std::sin(ph), 20, 5, 0}, {-15 * std::sin(ph), 20, 5, 0}));
    }
    std::vector<const TrackedBody*> ptrs;
    for (auto& f : frames) ptrs.push_back(&f);
    ptrs[10] = nullptr;  // untracked frame
    const std::vector<int> signs = anat::forwardSignsPerFrame(ptrs, 1, 15);
    // rgb: ml = +z, ant = +x  =>  ml x up = -x, so walking +x is sign -1
    expect(signs.size() == 120, "one sign per frame");
    int wrong = 0;
    for (int i = 0; i < 120; ++i) {
        if (i > 50 && i < 70) continue;  // the turn itself
        const int expected = i < 60 ? -1 : 1;
        if (signs[i] != expected) ++wrong;
    }
    expect(wrong == 0, "per-frame forward sign follows the turn (" + std::to_string(wrong) + " wrong)");
    const std::vector<int> none = anat::forwardSignsPerFrame({nullptr, nullptr}, -1);
    expect(none.size() == 2 && none[0] == -1 && none[1] == -1, "no evidence: fallback sign");
}

void testGapFilling() {
    std::vector<TrackedBody> frames;
    std::vector<double> times;
    for (int i = 0; i < 20; ++i) {
        frames.push_back(makeBody(kinectFacingCamera(), {2.0f * i, 10, 0, 0}, {0, 5, 0, 0}));
        times.push_back(i / 30.0);
    }
    const auto& L = anat::legJoints(anat::Side::Left);
    const TrackedBody truth = frames[5];
    frames[5].joints[L.foot].confidence = 0.0f;           // 1-frame dropout
    frames[5].joints[L.foot].x = 0;                       // garbage position
    for (int i = 10; i < 19; ++i) frames[i].joints[L.knee].confidence = 0.0f;  // 0.3 s: too long
    frames[3].joints[L.ankle].z = std::numeric_limits<float>::quiet_NaN();     // NaN counts as a gap
    std::vector<TrackedBody*> ptrs;
    for (auto& f : frames) ptrs.push_back(&f);
    const int filled = anat::fillTrackingGaps(ptrs, times, 0.25);
    expect(filled == 2, "fills the short gaps only (filled " + std::to_string(filled) + ")");
    expectNear(bf::length(jp(frames[5], L.foot) - jp(truth, L.foot)) * 1000, 0, 2.0, "interpolated foot is close to truth");
    expect(frames[5].joints[L.foot].confidence > 0.1f && frames[5].joints[L.foot].confidence < 0.5f,
           "filled joint is tracked, at low confidence");
    expect(std::isfinite(frames[3].joints[L.ankle].z), "NaN gap filled");
    expect(frames[14].joints[L.knee].confidence == 0.0f, "long gap left alone");

    // absent subject breaks a gap
    std::vector<TrackedBody*> withHole = ptrs;
    frames[7].joints[L.hip].confidence = 0.0f;
    withHole[8] = nullptr;
    frames[9].joints[L.hip].confidence = 1.0f;
    anat::fillTrackingGaps(withHole, times, 0.25);
    expect(frames[7].joints[L.hip].confidence == 0.0f, "gaps are not bridged across an absent subject");
}

void testTemporalLimits() {
    // Alternating a huge correction every frame: with state, vertical change
    // between frames is bounded by maxVerticalSpeed * dt.
    anat::Settings s = settingsFor(1);
    s.maxVerticalSpeed = 0.5f;
    s.limbs[0].enabled = s.limbs[1].enabled = true;
    s.limbs[0].mode = s.limbs[1].mode = anat::LimbMode::StepLength;
    s.limbs[0].stepGain = s.limbs[1].stepGain = 1.4f;
    anat::TemporalState st;
    float prevDrop = 0, prevGround = 0;
    bool first = true;
    float maxDropStep = 0, maxGroundStep = 0;
    for (int i = 0; i < 60; ++i) {
        const bool heelStrike = (i / 3) % 2 == 0;  // switch pose every 3 frames
        TrackedBody b = heelStrike ? makeBody(kinectFacingCamera(), {28, 3, 0, 0}, {-15, 8, 0, 0})
                                   : makeBody(kinectFacingCamera(), {5, 10, 0, 0}, {0, 5, 0, 0});
        if (i % 7 == 3) b.joints[anat::legJoints(anat::Side::Right).foot].confidence = 0.0f;
        anat::BodyDiagnostics d;
        const TrackedBody out = anat::apply(b, s, &d, &st, i / 30.0);
        expect(allFinite(out), "temporal: finite");
        if (!first) {
            maxDropStep = std::max(maxDropStep, std::fabs(d.pelvisDrop - prevDrop));
            maxGroundStep = std::max(maxGroundStep, std::fabs(d.groundShift - prevGround));
        }
        prevDrop = d.pelvisDrop;
        prevGround = d.groundShift;
        first = false;
    }
    const float limit = 0.5f / 30.0f + 1e-5f;
    expect(maxDropStep <= limit, "pelvis drop rate-limited (" + std::to_string(maxDropStep * 1000) + " mm/frame)");
    expect(maxGroundStep <= limit, "ground shift rate-limited (" + std::to_string(maxGroundStep * 1000) + " mm/frame)");

    // A time gap or a different body resets the state (no limiting across it)
    anat::TemporalState st2;
    const TrackedBody a = makeBody(kinectFacingCamera(), {5, 10, 0, 0}, {0, 5, 0, 0});
    TrackedBody b = makeBody(kinectFacingCamera(), {28, 3, 0, 0}, {-15, 8, 0, 0});
    anat::BodyDiagnostics d1, d2;
    anat::apply(a, s, &d1, &st2, 0.0);
    anat::apply(b, s, &d2, &st2, 5.0);  // 5 s later
    anat::BodyDiagnostics free;
    anat::apply(b, s, &free);
    expectNear(d2.pelvisDrop, free.pelvisDrop, 1e-6, "state resets after a time gap");
}

void testOffPlaneMetric() {
    // Abducted thigh: knee flexion moves the ankle sideways (its plane is
    // tilted), but that is not off-plane movement. The old three-point
    // rotation at standing is.
    const TrackedBody b = makeBody(kinectFacingCamera(), {20, 30, 5, 12}, {0, 3, 0, 0});
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    s.limbs[0].enabled = true;
    s.limbs[0].kneeGain = 1.6f;
    anat::BodyDiagnostics d;
    anat::apply(b, s, &d);
    expect(std::fabs(d.limbs[0].lateralShift) > 0.005f, "tilted knee plane does move the ankle sideways");
    expectNear(d.limbs[0].offPlane * 1000, 0, 0.05, "...but knee flexion is not off-plane");
    s.limbs[0].kneeGain = 1.0f;
    s.limbs[0].hipGain = 1.6f;
    s.limbs[0].ankleGain = 1.4f;
    anat::apply(b, s, &d);
    expectNear(d.limbs[0].offPlane * 1000, 0, 0.05, "hip and ankle flexion are not off-plane");

    const TrackedBody standing = makeBody(kinectFacingCamera(), {2, 2, 0, 0}, {2, 2, 0, 0});
    BiofeedbackEngine legacy;
    legacy.setTransform(BiomechAngle::LEFT_HIP_FLEXION, 1.5f, 1.5f, 0, 0);
    FrameData f;
    f.bodies.push_back(standing);
    const TrackedBody legacyOut = legacy.applyTransforms(f, 0).bodies[0];
    const anat::BodyDiagnostics ld = anat::compare(standing, legacyOut, s);
    expect(ld.limbs[0].offPlane > 0.05f, "old engine's sideways swing is flagged off-plane (" +
                                             std::to_string(ld.limbs[0].offPlane * 1000) + " mm)");
}

void testSwapLeftRight() {
    const TrackedBody b = makeBody(kinectFacingCamera(), {25, 40, 10, 0}, {-12, 5, -8, 0});
    TrackedBody s = b;
    anat::swapLeftRight(s);
    const auto& L = anat::legJoints(anat::Side::Left);
    const auto& R = anat::legJoints(anat::Side::Right);
    expectNear(bf::length(jp(s, L.knee) - jp(b, R.knee)), 0, 0, "swap: left knee <- right knee");
    expectNear(bf::length(jp(s, 5) - jp(b, 12)), 0, 0, "swap: shoulders too");
    expectNear(bf::length(jp(s, 0) - jp(b, 0)), 0, 0, "swap: midline joints untouched");
    anat::swapLeftRight(s);
    double err = 0;
    for (int j = 0; j < JOINT_COUNT; ++j) err += bf::length(jp(s, j) - jp(b, j));
    expectNear(err, 0, 0, "swap twice = identity");
    // a swapped body flips the forward vote (that's how swaps are detected)
    TrackedBody once = b;
    anat::swapLeftRight(once);
    int u1 = 0, u2 = 0;
    const double v1 = anat::forwardVote(b, u1), v2 = anat::forwardVote(once, u2);
    expect(u1 == 2 && u2 == 2 && v1 > 1.0 && v2 < -1.0, "swapped labels flip the forward vote");
}

// Overground walking with the stance ankle fixed in the world: an inverted
// pendulum stance leg and a flexing swing leg, 3 steps per side.
std::vector<TrackedBody> walkingSequence(std::vector<double>& times, std::vector<int>& stanceSide,
                                         int standStillFrames = 0) {
    std::vector<TrackedBody> frames;
    const Convention c = kinectFacingCamera();
    const int perStep = 18;  // 0.6 s at 30 fps
    Vec3 plant(0.1f, -0.85f, 2.5f);
    bool first = true;
    int side = 0;
    for (int step = 0; step < 6; ++step, side = 1 - side) {
        TrackedBody last;
        for (int k = 0; k < perStep; ++k) {
            const float u = k / (float)perStep;
            const float stanceHip = 20 - 40 * u, swingHip = -20 + 40 * u;
            const float swingKnee = 40 * std::sin(3.14159f * u);
            // feet kept level (stance) / toes slightly up (swing), as in real gait —
            // nothing pokes below the floor the stance foot stands on
            const LegPose st{stanceHip, 2, -(stanceHip - 2), 0};
            const LegPose sw{swingHip, swingKnee + 2, -(swingHip - swingKnee - 2) + 10, 0};
            TrackedBody b = side == 0 ? makeBody(c, st, sw) : makeBody(c, sw, st);
            const auto& L = anat::legJoints((anat::Side)side);
            if (first) { plant = Vec3(jp(b, L.ankle).x, -0.85f, jp(b, L.ankle).z); first = false; }
            const Vec3 shift = plant - jp(b, L.ankle);
            for (auto& jt : b.joints) { jt.x += shift.x; jt.y += shift.y; jt.z += shift.z; }
            frames.push_back(b);
            times.push_back(frames.size() / 30.0);
            stanceSide.push_back(side);
            last = b;
        }
        // next stance foot lands where the swing ankle would be at u = 1
        const LegPose st{-20, 2, 0, 0}, sw{20, 2, 0, 0};
        TrackedBody end = side == 0 ? makeBody(c, st, sw) : makeBody(c, sw, st);
        const auto& Ls = anat::legJoints((anat::Side)side);
        const auto& Lw = anat::legJoints((anat::Side)(1 - side));
        const Vec3 off = plant - jp(end, Ls.ankle);
        plant = jp(end, Lw.ankle) + off;
        plant.y = -0.85f;
    }
    for (int k = 0; k < standStillFrames; ++k) {
        frames.push_back(frames.back());
        times.push_back(frames.size() / 30.0);
        stanceSide.push_back(-1);
    }
    return frames;
}

void testStanceFootPlanting() {
    std::vector<double> times;
    std::vector<int> stance;
    const auto frames = walkingSequence(times, stance, 90);
    anat::Settings s = settingsFor(1);
    for (auto& l : s.limbs) { l.enabled = true; l.hipGain = 1.5f; }

    auto run = [&](bool plant, float& maxSlip, float& travelIn, float& travelOut, float& finalOffset) {
        s.plantStanceFoot = plant;
        anat::TemporalState st;
        std::vector<TrackedBody> out;
        for (size_t i = 0; i < frames.size(); ++i) {
            anat::BodyDiagnostics d;
            out.push_back(anat::apply(frames[i], s, &d, &st, times[i]));
            finalOffset = bf::length(d.rootOffset);
        }
        maxSlip = 0;
        for (size_t i = 1; i < frames.size(); ++i) {
            if (stance[i] < 0 || stance[i] != stance[i - 1]) continue;
            if (i >= 2 && stance[i - 2] != stance[i]) continue;  // contact settling
            const auto& L = anat::legJoints((anat::Side)stance[i]);
            Vec3 d = jp(out[i], L.ankle) - jp(out[i - 1], L.ankle);
            d.y = 0;
            maxSlip = std::max(maxSlip, bf::length(d));
        }
        const size_t walkEnd = frames.size() - 91;
        Vec3 ti = jp(frames[walkEnd], 0) - jp(frames[0], 0), to = jp(out[walkEnd], 0) - jp(out[0], 0);
        ti.y = to.y = 0;
        travelIn = bf::length(ti);
        travelOut = bf::length(to);
    };
    float slipOff, inOff, outOff, endOff, slipOn, inOn, outOn, endOn;
    run(false, slipOff, inOff, outOff, endOff);
    run(true, slipOn, inOn, outOn, endOn);
    expect(slipOff > 0.01f, "without planting the stance foot slides (" + std::to_string(slipOff * 1000) + " mm/frame)");
    expect(slipOn < 0.002f, "with planting the stance foot stays put (" + std::to_string(slipOn * 1000) + " mm/frame)");
    expect(outOn > inOn * 1.15f, "planted avatar travels further than the person (" + std::to_string(outOn) + " vs " +
                                     std::to_string(inOn) + " m)");
    expectNear(outOff, inOff, 0.02, "unplanted avatar stays with the person");
    expect(endOn < 0.6f * (outOn - inOn) + 0.01f, "standing still afterwards: the avatar drifts back toward the person");
}

// ---------------------------------------------------------------------------
// LiveProcessor (causal pipeline for the live patient screen)
// ---------------------------------------------------------------------------

// Rotate a body 180° about the vertical through its pelvis and swap its
// left/right labels: what a facing-the-camera tracker reports when the
// subject turns their back.
TrackedBody facingAwayAsKinectSeesIt(const TrackedBody& b) {
    TrackedBody r = b;
    const Vec3 p = jp(b, 0);
    for (auto& jt : r.joints) {
        jt.x = 2 * p.x - jt.x;
        jt.z = 2 * p.z - jt.z;
    }
    anat::swapLeftRight(r);
    return r;
}

void testLiveGapHolding() {
    anat::LiveProcessor lp;
    anat::Settings s = settingsFor(1);
    const auto& L = anat::legJoints(anat::Side::Left);
    TrackedBody truth3;
    for (int i = 0; i < 30; ++i) {
        TrackedBody b = makeBody(kinectFacingCamera(), {2.0f * i, 10, 0, 0}, {0, 5, 0, 0});
        if (i == 3) truth3 = b;
        if (i >= 3 && i <= 5) b.joints[L.foot].confidence = 0.0f;          // 0.1 s: held
        if (i >= 10 && i <= 29) b.joints[L.knee].confidence = 0.0f;        // long: let go
        lp.process(b, i / 30.0, s);
        if (i == 3) {
            const Vec3 held = jp(lp.lastInput(), L.foot);
            expect(lp.lastInput().joints[L.foot].confidence > 0.1f, "live: short foot dropout held");
            expectNear(bf::length(held - jp(truth3, L.foot)) * 1000, 0, 25, "live: held foot follows the ankle");
        }
        if (i == 25)
            expect(lp.lastInput().joints[L.knee].confidence < 0.1f, "live: long dropout not held past 0.25 s");
    }
}

void testLiveSwapCorrection() {
    anat::LiveProcessor lp;
    anat::LiveProcessor::Options o;
    o.forwardHint = 1;  // Kinect: left-handed coordinates
    lp.setOptions(o);
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    int swappedFrames = 0, correctLeft = 0, checked = 0;
    for (int i = 0; i < 150; ++i) {
        const float ph = i * 0.21f;
        // left and right legs clearly different: left forward, right back
        const TrackedBody facing = makeBody(kinectFacingCamera(), {25 + 3 * std::sin(ph), 10, 0, 0},
                                            {-10 + 3 * std::sin(ph), 5, 0, 0});
        const bool away = i >= 50 && i < 110;
        const TrackedBody b = away ? facingAwayAsKinectSeesIt(facing) : facing;
        lp.process(b, i / 30.0, s);
        if (lp.status().swapped) ++swappedFrames;
        if ((i >= 75 && i < 110) || (i >= 135)) {  // ~0.8 s after each turn
            ++checked;
            const anat::BodyAxes ax = anat::computeAxes(lp.lastInput(), s);
            const anat::LegMeasure m = anat::measureLeg(lp.lastInput(), anat::Side::Left, ax);
            if (m.valid && m.hip > 15 * kDeg) ++correctLeft;
        }
    }
    expect(swappedFrames >= 35 && swappedFrames <= 62, "live: swap detected while facing away (" +
                                                           std::to_string(swappedFrames) + " frames)");
    expect(correctLeft == checked, "live: the anatomical left leg stays 'left' (" + std::to_string(correctLeft) + "/" +
                                       std::to_string(checked) + ")");

    // without the fix the left leg reads as the right one while facing away
    anat::LiveProcessor raw;
    o.fixLeftRightSwaps = false;
    raw.setOptions(o);
    TrackedBody facing = makeBody(kinectFacingCamera(), {25, 10, 0, 0}, {-10, 5, 0, 0});
    for (int i = 0; i < 40; ++i) raw.process(facingAwayAsKinectSeesIt(facing), i / 30.0, s);
    expect(!raw.status().swapped, "live: swap fix can be switched off");
}

void testLivePlanarTurning() {
    anat::LiveProcessor lp;
    anat::Settings s = settingsFor(1);
    int wrong = 0;
    for (int i = 0; i < 180; ++i) {
        Convention c = rgbSideView();
        const bool back = (i / 60) % 2 == 1;  // turns round every 2 s
        if (back) c.ant = Vec3(-1, 0, 0);
        const float ph = i * 0.2f;
        const TrackedBody b = makeBody(c, {15 * std::sin(ph), 20, 5, 0}, {-15 * std::sin(ph), 20, 5, 0});
        lp.process(b, i / 30.0, s);
        const int expected = back ? 1 : -1;  // rgb: ml x up = -x
        if (i % 60 >= 25 && lp.status().forwardSign != expected) ++wrong;  // allow ~0.8 s to follow
    }
    expect(lp.status().planar, "live: 2D detected");
    expect(wrong == 0, "live: 2D forward sign follows the subject turning (" + std::to_string(wrong) + " wrong)");
}

void testLiveLocomotion() {
    std::vector<double> times;
    std::vector<int> stance;
    const auto walk = walkingSequence(times, stance, 0);
    anat::LiveProcessor lp;
    anat::Settings s = settingsFor(1);
    for (auto& l : s.limbs) { l.enabled = true; l.hipGain = 1.5f; }
    bool overground = false;
    for (size_t i = 0; i < walk.size(); ++i) {
        lp.process(walk[i], times[i], s);
        overground = overground || lp.status().overground;
    }
    expect(overground, "live: overground walking detected");

    anat::LiveProcessor tm;  // treadmill: pelvis stays put
    bool any = false;
    for (int i = 0; i < 200; ++i) {
        const float ph = i * 0.18f;
        tm.process(makeBody(kinectFacingCamera(), {20 * std::sin(ph), 15, 0, 0}, {-20 * std::sin(ph), 15, 0, 0}),
                   i / 30.0, s);
        any = any || tm.status().overground;
    }
    expect(!any, "live: treadmill walking is not overground");
}

void testLiveMatchesTargets() {
    // On clean data the live pipeline must hit the same angle targets as the engine
    anat::LiveProcessor lp;
    anat::LiveProcessor::Options o;
    o.forwardHint = 1;
    lp.setOptions(o);
    anat::Settings s = settingsFor(1);
    s.groundContact = false;
    s.limbs[0].enabled = true;
    s.limbs[0].hipGain = 1.4f;
    s.limbs[0].kneeGain = 1.3f;
    double worst = 0;
    for (int i = 0; i < 120; ++i) {
        const float ph = i * 0.2f;
        const TrackedBody b = makeBody(kinectFacingCamera(), {15 + 20 * std::sin(ph), 30 + 25 * std::sin(ph + 1), 0, 0},
                                       {5, 5, 0, 0});
        anat::BodyDiagnostics d;
        lp.process(b, i / 30.0, s, &d);
        if (d.limbs[0].valid) {
            worst = std::max(worst, (double)std::fabs(d.limbs[0].out.hip - d.limbs[0].target[0]));
            worst = std::max(worst, (double)std::fabs(d.limbs[0].out.knee - d.limbs[0].target[1]));
        }
    }
    expectNear(worst / kDeg, 0, 0.05, "live: angle targets hit");
}

// ---------------------------------------------------------------------------
// Randomised invariants (run under ASan/UBSan locally)
// ---------------------------------------------------------------------------

void testRandomisedInvariants() {
    const char* env = std::getenv("ANAT_FUZZ_ITERS");
    const int iters = env ? std::atoi(env) : 3000;
    std::mt19937 rng(12345);
    auto U = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };
    auto coin = [&](float p) { return U(0, 1) < p; };
    const Convention convs[3] = {kinectFacingCamera(), zedFacingCamera(), rgbSideView()};
    const int fwd[3] = {1, -1, -1};
    int failuresBefore = g_failures;

    for (int it = 0; it < iters && g_failures - failuresBefore < 10; ++it) {
        const int ci = it % 3;
        auto randomLeg = [&]() {
            return LegPose{U(-35, 120), U(-15, 150), U(-50, 40), ci == 2 ? 0.0f : U(-15, 25)};
        };
        TrackedBody b = makeBody(convs[ci], randomLeg(), randomLeg());
        // tracking dropouts
        for (int j = 0; j < JOINT_COUNT; ++j)
            if (coin(0.04f)) b.joints[j].confidence = 0.0f;
        if (coin(0.03f)) b.joints[(int)(U(0, JOINT_COUNT - 0.01f))].x = std::numeric_limits<float>::quiet_NaN();

        anat::Settings s = settingsFor(fwd[ci]);
        s.groundContact = coin(0.5f);
        s.pelvisDrop = coin(0.5f);
        s.hipReference = coin(0.5f) ? anat::HipReference::Pelvis : anat::HipReference::WorldVertical;
        s.limitToJointRanges = coin(0.8f);
        for (int side = 0; side < 2; ++side) {
            auto& ls = s.limbs[side];
            ls.enabled = coin(0.8f);
            ls.mode = coin(0.3f) ? anat::LimbMode::StepLength : anat::LimbMode::JointAngles;
            ls.hipGain = coin(0.2f) ? 1.0f : U(0, 3);
            ls.kneeGain = coin(0.2f) ? 1.0f : U(0, 3);
            ls.ankleGain = coin(0.5f) ? 1.0f : U(0, 3);
            ls.stepGain = U(0.3f, 2.5f);
            if (coin(0.3f)) s.neutral[side] = {U(-10, 15) * kDeg, U(0, 15) * kDeg, U(-10, 10) * kDeg, U(-0.1f, 0.1f)};
        }

        anat::BodyDiagnostics d;
        const TrackedBody out = anat::apply(b, s, &d);
        const std::string tag = " [iter " + std::to_string(it) + "]";

        // 1. finite in -> finite out, for every joint
        for (int j = 0; j < JOINT_COUNT; ++j) {
            const bool finIn = std::isfinite(b.joints[j].x) && std::isfinite(b.joints[j].y) && std::isfinite(b.joints[j].z);
            const bool finOut = std::isfinite(out.joints[j].x) && std::isfinite(out.joints[j].y) && std::isfinite(out.joints[j].z);
            if (finIn && !finOut) { expect(false, "finite joint became non-finite" + tag); break; }
        }
        // 2. the trunk and arms are only ever translated (never rotated or stretched)
        {
            const int trunk[] = {0, 1, 2, 3, 5, 12, 26};
            const Vec3 off = jp(out, 0) - jp(b, 0);
            for (int j : trunk) {
                if (!std::isfinite(b.joints[j].x) || !std::isfinite(b.joints[0].x)) continue;
                if (bf::length((jp(out, j) - jp(b, j)) - off) > 1e-4f) { expect(false, "trunk deformed" + tag); break; }
            }
        }
        // 3. leg bone lengths preserved
        for (int side = 0; side < 2; ++side) {
            const auto& L = anat::legJoints((anat::Side)side);
            for (auto [a, c2] : {std::pair{L.hip, L.knee}, std::pair{L.knee, L.ankle}, std::pair{L.ankle, L.foot}}) {
                const float li = boneLen(b, a, c2), lo = boneLen(out, a, c2);
                if (std::isfinite(li) && std::fabs(li - lo) > 1e-4f) { expect(false, "bone length changed" + tag); break; }
            }
        }
        // 4. angle-mode targets are hit exactly (when the body wasn't lowered for IK)
        if (d.valid && d.pelvisDrop < 1e-6f) {
            for (int side = 0; side < 2; ++side) {
                const auto& ld = d.limbs[side];
                const auto& ls = s.limbs[side];
                if (!ls.enabled || !ld.valid || ls.mode != anat::LimbMode::JointAngles) continue;
                auto detail = [&]() {
                    return " side=" + std::to_string(side) + " conv=" + std::to_string(ci) +
                           " in(h,k)=" + std::to_string(ld.in.hip / kDeg) + "," + std::to_string(ld.in.knee / kDeg) +
                           " tgt=" + std::to_string(ld.target[0] / kDeg) + "," + std::to_string(ld.target[1] / kDeg) +
                           " out=" + std::to_string(ld.out.hip / kDeg) + "," + std::to_string(ld.out.knee / kDeg) +
                           " ground=" + std::to_string(d.groundShift) + " gc=" + std::to_string(s.groundContact) +
                           " ref=" + std::to_string((int)s.hipReference);
                };
                // with joint limits off a target can pass 180°; measured angles wrap
                auto miss = [](float out, float target) {
                    return std::fabs(std::remainder(out - target, 2.0f * 3.14159265f)) > 0.002f;
                };
                if (miss(ld.out.hip, ld.target[0])) { expect(false, "hip target missed" + tag + detail()); break; }
                if (miss(ld.out.knee, ld.target[1])) { expect(false, "knee target missed" + tag + detail()); break; }
                if (ld.in.ankleValid && ld.out.ankleValid && std::isfinite(ld.target[2]) &&
                    miss(ld.out.ankle, ld.target[2])) { expect(false, "ankle target missed" + tag); break; }
                if (ld.offPlane > 1e-4f) { expect(false, "angle mode moved a limb off its flexion plane" + tag); break; }
                // hip/knee gains never move the ankle sideways relative to the hip by more
                // than the knee's own (tilted) flexion plane allows
                if (std::fabs(ls.kneeGain - 1.0f) < 1e-4f && std::fabs(ld.lateralShift) > 1e-4f) {
                    expect(false, "hip gain moved the ankle sideways" + tag);
                    break;
                }
            }
        }
        // 5. ground contact keeps the lowest foot point
        if (s.groundContact && d.valid) {
            float lowIn = 1e9f, lowOut = 1e9f;
            bool anyIn = false;
            for (int j : {20, 21, 24, 25}) {
                const Joint& jb = b.joints[j];
                if (jb.confidence < 0.1f || !std::isfinite(jb.x) || !std::isfinite(jb.y) || !std::isfinite(jb.z)) continue;
                anyIn = true;
                lowIn = std::min(lowIn, b.joints[j].y);
                lowOut = std::min(lowOut, out.joints[j].y);
            }
            if (anyIn && std::fabs(lowIn - lowOut) > 1e-4f)
                expect(false, "ground contact lost" + tag + " in=" + std::to_string(lowIn) + " out=" + std::to_string(lowOut) +
                                  " conv=" + std::to_string(ci));
        }
        // 6. diagnostics are finite when valid
        for (int side = 0; side < 2 && d.valid; ++side) {
            const auto& ld = d.limbs[side];
            if (ld.valid && (!std::isfinite(ld.out.hip) || !std::isfinite(ld.lateralShift) ||
                             !std::isfinite(ld.boneLengthError)))
                expect(false, "non-finite diagnostic" + tag);
        }
    }
}

} // namespace

int main() {
    testForwardSignDetection();
    testMeasuresSignedAngles();
    testUnitGainIsIdentity();
    testHipGainOnlyMovesSagittally();
    testExtensionStaysExtension();
    testStraightKneeIsStable();
    testNeutralCalibration();
    testGroundContact();
    testStepLengthMode();
    testPlanarDataStaysInPlane();
    testLowConfidenceFootFollowsChain();
    testNonFiniteJointsAreIgnored();
    testNonFiniteGainIsIgnored();
    testJointRangeLimits();
    testGainZeroFreezesAtNeutral();
    testOneLegStepModeKeepsOtherFootPlanted();
    testTiltedFloor();
    testTreadmillFloorIsNotTilted();
    testForwardSignsPerFrame();
    testGapFilling();
    testTemporalLimits();
    testOffPlaneMetric();
    testSwapLeftRight();
    testStanceFootPlanting();
    testLiveGapHolding();
    testLiveSwapCorrection();
    testLivePlanarTurning();
    testLiveLocomotion();
    testLiveMatchesTargets();
    testRandomisedInvariants();

    if (g_failures == 0) {
        std::cout << "anatomical_engine_tests passed\n";
        return 0;
    }
    std::cerr << g_failures << " anatomical_engine_tests failure(s)\n";
    return 1;
}
