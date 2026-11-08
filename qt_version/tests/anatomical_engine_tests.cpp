// Unit tests for the experimental anatomical biofeedback engine.
// Pure C++ (no Qt) — builds synthetic legs in both Kinect-style (left-handed)
// and ZED-style (right-handed) coordinates and checks the engine's guarantees:
// signed angles, gains about neutral, abduction preserved, bone lengths kept,
// feet kept on the floor, no NaNs at full extension.

#include "../src/biofeedback/anatomical_engine.h"

#include <algorithm>
#include <cmath>
#include <iostream>
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
            expectNear(out.joints[L.ankle].y, b.joints[L.ankle].y, 0.001, n + ": ankle height kept");
            expectNear(d.limbs[side].lateralShift * 1000, 0, 0.05, n + ": no sideways movement");
        }
        if (drop) {
            expect(d.pelvisDrop > 0.001f, n + ": pelvis lowered");
            expectNear(d.limbs[0].out.stepAP, 1.3 * d.limbs[0].in.stepAP, 0.001, n + ": front step scaled 1.3x");
            expectNear(d.limbs[1].out.stepAP, 1.3 * d.limbs[1].in.stepAP, 0.001, n + ": rear step scaled 1.3x");
        } else {
            expectNear(d.pelvisDrop, 0, 1e-6, n + ": pelvis not lowered");
            // A near-straight leg is already at full reach: without dropping the
            // body the step can't get longer — but it must never get shorter
            expect(d.limbs[0].out.stepAP >= d.limbs[0].in.stepAP - 1e-4f, n + ": never shorter than input");
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

    if (g_failures == 0) {
        std::cout << "anatomical_engine_tests passed\n";
        return 0;
    }
    std::cerr << g_failures << " anatomical_engine_tests failure(s)\n";
    return 1;
}
