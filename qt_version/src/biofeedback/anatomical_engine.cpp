#include "anatomical_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>

using bf::Quat;
using bf::Vec3;

namespace anat {

namespace {

const Vec3 kWorldUp(0.0f, 1.0f, 0.0f);
constexpr float kGainEps = 1e-4f;
constexpr float kReach = 0.9995f;  // fraction of full leg length IK may use

const LegJointIndices kLegs[2] = {
    {(int)CanonicalJoint::HIP_LEFT, (int)CanonicalJoint::KNEE_LEFT,
     (int)CanonicalJoint::ANKLE_LEFT, (int)CanonicalJoint::FOOT_LEFT},
    {(int)CanonicalJoint::HIP_RIGHT, (int)CanonicalJoint::KNEE_RIGHT,
     (int)CanonicalJoint::ANKLE_RIGHT, (int)CanonicalJoint::FOOT_RIGHT},
};

Vec3 pos(const TrackedBody& b, int j) {
    const Joint& p = b.joints[j];
    return {p.x, p.y, p.z};
}

void setPos(TrackedBody& b, int j, const Vec3& v) {
    b.joints[j].x = v.x;
    b.joints[j].y = v.y;
    b.joints[j].z = v.z;
    b.joints2D[j].valid = false;  // image-space projection is stale now
}

bool tracked(const TrackedBody& b, int j, float minConf) {
    return b.joints[j].confidence >= minConf;
}

void translate(TrackedBody& b, const Vec3& delta) {
    for (int j = 0; j < JOINT_COUNT; ++j) setPos(b, j, pos(b, j) + delta);
}

bool isPlanar(const TrackedBody& b, float minConf) {
    bool any = false;
    for (int j = 0; j < JOINT_COUNT; ++j) {
        if (!tracked(b, j, minConf)) continue;
        any = true;
        if (std::fabs(b.joints[j].z) > 1e-5f) return false;
    }
    return any;
}

// Frame of a limb segment: d = long axis (proximal → distal), m = its
// medio-lateral (flexion) axis, a = its anterior direction. Built so that at
// neutral (d = -up, m = ml) a equals the pelvis anterior axis.
struct SegmentFrame {
    Vec3 d, m, a;
};

SegmentFrame segmentFrame(const Vec3& distal, const Vec3& mlRef, int forwardSign) {
    SegmentFrame f;
    f.d = distal;
    f.m = bf::normalizeOr(bf::rejectFrom(mlRef, distal), mlRef);
    f.a = bf::normalizeOr(bf::cross(f.m, f.d) * (-(float)forwardSign), Vec3(0, 0, 1));
    return f;
}

// The plane a joint flexes in, as an orthonormal pair (u, w): the joint angle
// is atan2(v·w, v·u) for the distal bone v, and rotating about u × w by +a
// increases it by a.
struct JointPlane {
    bool valid = false;
    Vec3 centre, u, w;
    int distal = -1;
    int chain[3] = {-1, -1, -1};
    int chainLen = 0;
};

JointPlane jointPlane(const TrackedBody& b, Side side, LegJoint joint, const BodyAxes& axes) {
    JointPlane p;
    if (!axes.valid) return p;
    const LegJointIndices& L = kLegs[(int)side];
    const Vec3 H = pos(b, L.hip), K = pos(b, L.knee), T = pos(b, L.ankle);

    switch (joint) {
        case LegJoint::Hip:
            p.centre = H;
            p.u = -axes.up;
            p.w = axes.ant;
            p.distal = L.knee;
            p.chain[0] = L.knee; p.chain[1] = L.ankle; p.chain[2] = L.foot;
            p.chainLen = 3;
            p.valid = true;
            break;
        case LegJoint::Knee: {
            Vec3 dThigh;
            if (!bf::tryNormalize(K - H, dThigh)) return p;
            const SegmentFrame thigh = segmentFrame(dThigh, axes.ml, axes.forwardSign);
            p.centre = K;
            p.u = thigh.d;
            p.w = -thigh.a;  // shank swinging posteriorly = flexion
            p.distal = L.ankle;
            p.chain[0] = L.ankle; p.chain[1] = L.foot;
            p.chainLen = 2;
            p.valid = true;
            break;
        }
        case LegJoint::Ankle: {
            Vec3 dThigh, dShank;
            if (!bf::tryNormalize(K - H, dThigh) || !bf::tryNormalize(T - K, dShank)) return p;
            const SegmentFrame thigh = segmentFrame(dThigh, axes.ml, axes.forwardSign);
            const SegmentFrame shank = segmentFrame(dShank, thigh.m, axes.forwardSign);
            p.centre = T;
            p.u = shank.a;   // foot pointing forward = 0
            p.w = -shank.d;  // toes up = dorsiflexion
            p.distal = L.foot;
            p.chain[0] = L.foot;
            p.chainLen = 1;
            p.valid = true;
            break;
        }
    }
    return p;
}

float planeAngle(const JointPlane& p, const Vec3& v) {
    return std::atan2(bf::dot(v, p.w), bf::dot(v, p.u));
}

// Rotate everything distal to `joint` so its angle becomes `target`. The whole
// chain is rotated (not just tracked joints) so a briefly low-confidence foot
// can't be left behind, detached from the ankle.
void setJointAngle(TrackedBody& b, Side side, LegJoint joint, float target, const BodyAxes& axes) {
    const JointPlane p = jointPlane(b, side, joint, axes);
    if (!p.valid) return;
    const float delta = target - planeAngle(p, pos(b, p.distal) - p.centre);
    if (std::fabs(delta) < 1e-7f) return;
    const Quat q = Quat::fromAxisAngle(bf::cross(p.u, p.w), delta);
    for (int i = 0; i < p.chainLen; ++i) {
        const int j = p.chain[i];
        setPos(b, j, p.centre + q.rotate(pos(b, j) - p.centre));
    }
}

// Two-bone IK: place the ankle at `target` keeping thigh and shank lengths.
// The knee stays on the side it was bending toward (anterior if the leg was
// dead straight). The foot keeps its orientation in the world — IK is only
// used where the foot should stay planted, and a planted foot doesn't rotate,
// so the ankle angle absorbs the change (as it does in a real longer stride).
void solveLegIK(TrackedBody& b, Side side, const Vec3& target, const Vec3& anterior) {
    const LegJointIndices& L = kLegs[(int)side];
    const Vec3 H = pos(b, L.hip), K = pos(b, L.knee), T = pos(b, L.ankle), F = pos(b, L.foot);
    const float L1 = bf::length(K - H), L2 = bf::length(T - K);
    if (L1 < 1e-3f || L2 < 1e-3f) return;

    Vec3 w = target - H;
    float D = bf::length(w);
    if (D < 1e-6f) return;
    const float maxReach = (L1 + L2) * kReach;
    const float minReach = std::fabs(L1 - L2) + 1e-3f;
    if (D > maxReach) { w = w * (maxReach / D); D = maxReach; }
    if (D < minReach) { w = w * (minReach / D); D = minReach; }
    const Vec3 e = w / D;

    const Vec3 e0 = bf::normalizeOr(T - H, e);
    Vec3 pole = bf::rejectFrom((K - H) - e0 * bf::dot(K - H, e0), e);
    if (bf::length(pole) < 1e-3f) pole = bf::rejectFrom(anterior, e);
    pole = bf::normalizeOr(pole, Vec3(0, 0, 1));

    const float a = (L1 * L1 - L2 * L2 + D * D) / (2.0f * D);
    const float h = std::sqrt(std::max(0.0f, L1 * L1 - a * a));
    const Vec3 newK = H + e * a + pole * h;
    const Vec3 newT = H + w;

    setPos(b, L.knee, newK);
    setPos(b, L.ankle, newT);
    setPos(b, L.foot, newT + (F - T));
}

// If a scaled step is out of reach, shorten only its forward/back component so
// the foot keeps its height instead of being pulled up toward the hip — but
// never below the step the person actually took (`inputAP`).
Vec3 clampStepTarget(const Vec3& hip, const Vec3& target, float reach, float inputAP,
                     const BodyAxes& axes) {
    const Vec3 w = target - hip;
    if (bf::length(w) <= reach) return target;
    const float m = bf::dot(w, axes.ml), u = bf::dot(w, axes.up), a = bf::dot(w, axes.ant);
    const float room = reach * reach - m * m - u * u;
    if (room <= 0.0f) return target;  // solveLegIK falls back to pointing the leg at it
    const float limit = std::max(std::sqrt(room), std::fabs(inputAP));
    const float aClamped = std::copysign(std::min(std::fabs(a), limit), a);
    return hip + axes.ml * m + axes.up * u + axes.ant * aClamped;
}

bool lowestFootHeight(const TrackedBody& b, float minConf, float& out) {
    bool any = false;
    float low = std::numeric_limits<float>::max();
    for (const auto& L : kLegs) {
        for (int j : {L.ankle, L.foot}) {
            if (!tracked(b, j, minConf)) continue;
            low = std::min(low, b.joints[j].y);
            any = true;
        }
    }
    if (any) out = low;
    return any;
}

float gainOrOne(bool enabled, float gain) { return enabled ? gain : 1.0f; }

} // namespace

const LegJointIndices& legJoints(Side side) { return kLegs[(int)side]; }

const char* sideName(Side side) { return side == Side::Left ? "Left" : "Right"; }

const char* legJointName(LegJoint joint) {
    switch (joint) {
        case LegJoint::Hip: return "Hip";
        case LegJoint::Knee: return "Knee";
        case LegJoint::Ankle: return "Ankle";
    }
    return "?";
}

BodyAxes computeAxes(const TrackedBody& body, const Settings& settings) {
    BodyAxes ax;
    const float c = settings.minConfidence;
    const int hipL = (int)CanonicalJoint::HIP_LEFT, hipR = (int)CanonicalJoint::HIP_RIGHT;
    if (!tracked(body, hipL, c) || !tracked(body, hipR, c)) return ax;

    const Vec3 hl = pos(body, hipL), hr = pos(body, hipR);
    const int pelvis = (int)CanonicalJoint::PELVIS;
    ax.origin = tracked(body, pelvis, c) ? pos(body, pelvis) : (hl + hr) * 0.5f;
    ax.planar = isPlanar(body, c);

    if (ax.planar) {
        ax.ml = Vec3(0, 0, 1);  // 2D: flexion happens in the image plane
    } else if (!bf::tryNormalize(hr - hl, ax.ml)) {
        return ax;
    }

    Vec3 upRaw = kWorldUp;
    if (settings.hipReference == HipReference::Pelvis) {
        const int navel = (int)CanonicalJoint::SPINE_NAVEL, chest = (int)CanonicalJoint::SPINE_CHEST;
        if (tracked(body, navel, c)) upRaw = pos(body, navel) - ax.origin;
        else if (tracked(body, chest, c)) upRaw = pos(body, chest) - ax.origin;
    }
    if (!bf::tryNormalize(bf::rejectFrom(upRaw, ax.ml), ax.up) &&
        !bf::tryNormalize(bf::rejectFrom(kWorldUp, ax.ml), ax.up))
        return ax;

    ax.forwardSign = settings.forwardSign >= 0 ? 1 : -1;
    ax.ant = bf::cross(ax.ml, ax.up) * (float)ax.forwardSign;
    ax.valid = true;
    return ax;
}

LegMeasure measureLeg(const TrackedBody& body, Side side, const BodyAxes& axes, float minConf) {
    LegMeasure m;
    if (!axes.valid) return m;
    const LegJointIndices& L = kLegs[(int)side];
    if (!tracked(body, L.hip, minConf) || !tracked(body, L.knee, minConf) ||
        !tracked(body, L.ankle, minConf))
        return m;

    const JointPlane hip = jointPlane(body, side, LegJoint::Hip, axes);
    const JointPlane knee = jointPlane(body, side, LegJoint::Knee, axes);
    if (!hip.valid || !knee.valid) return m;

    const Vec3 H = pos(body, L.hip), K = pos(body, L.knee), T = pos(body, L.ankle);
    m.hip = planeAngle(hip, K - H);
    m.knee = planeAngle(knee, T - K);
    if (tracked(body, L.foot, minConf)) {
        const JointPlane ankle = jointPlane(body, side, LegJoint::Ankle, axes);
        if (ankle.valid) {
            m.ankle = planeAngle(ankle, pos(body, L.foot) - T);
            m.ankleValid = true;
        }
    }
    const Vec3 r = T - H;
    m.stepAP = bf::dot(r, axes.ant);
    m.stepML = bf::dot(r, axes.ml);
    m.stepUp = bf::dot(r, axes.up);
    m.valid = true;
    return m;
}

int detectForwardSign(const std::vector<const TrackedBody*>& bodies, float minConf) {
    Settings s;
    s.forwardSign = 1;
    s.minConfidence = minConf;
    double votes = 0.0;
    int count = 0;
    for (const TrackedBody* b : bodies) {
        if (!b) continue;
        const BodyAxes ax = computeAxes(*b, s);
        if (!ax.valid) continue;
        for (const auto& L : kLegs) {
            if (!tracked(*b, L.ankle, minConf) || !tracked(*b, L.foot, minConf)) continue;
            Vec3 f = bf::rejectFrom(pos(*b, L.foot) - pos(*b, L.ankle), ax.up);
            if (!ax.planar) f = bf::rejectFrom(f, ax.ml);
            const float len = bf::length(f);
            if (len < 0.02f) continue;  // foot seen end-on — no information
            votes += bf::dot(f, ax.ant) / len;
            ++count;
        }
    }
    if (count < 3 || std::fabs(votes) / count < 0.2) return 0;
    return votes > 0.0 ? 1 : -1;
}

void captureNeutral(const TrackedBody& body, Settings& settings) {
    const BodyAxes ax = computeAxes(body, settings);
    for (int s = 0; s < 2; ++s) {
        const LegMeasure m = measureLeg(body, (Side)s, ax, settings.minConfidence);
        if (!m.valid) continue;
        Neutral& n = settings.neutral[s];
        n.hip = m.hip;
        n.knee = m.knee;
        n.stepAP = m.stepAP;
        if (m.ankleValid) n.ankle = m.ankle;
    }
}

TrackedBody apply(const TrackedBody& in, const Settings& settings, BodyDiagnostics* diag) {
    TrackedBody out = in;
    const BodyAxes ax = computeAxes(in, settings);
    const bool anyEnabled = settings.limbs[0].enabled || settings.limbs[1].enabled;
    if (!ax.valid || !anyEnabled) {
        if (diag) *diag = compare(in, out, settings);
        return out;
    }

    const float c = settings.minConfidence;
    const LegMeasure mIn[2] = {measureLeg(in, Side::Left, ax, c), measureLeg(in, Side::Right, ax, c)};

    // --- Step mode: scaled ankle targets, and how far the body must drop to reach them
    Vec3 stepTarget[2];
    bool stepActive[2] = {false, false};
    float drop = 0.0f;
    for (int s = 0; s < 2; ++s) {
        const LimbSettings& ls = settings.limbs[s];
        if (!ls.enabled || ls.mode != LimbMode::StepLength || !mIn[s].valid) continue;
        const LegJointIndices& L = kLegs[s];
        const Neutral& n = settings.neutral[s];
        const Vec3 H = pos(in, L.hip);
        const float ap = n.stepAP + ls.stepGain * (mIn[s].stepAP - n.stepAP);
        stepTarget[s] = H + ax.ml * mIn[s].stepML + ax.up * mIn[s].stepUp + ax.ant * ap;
        stepActive[s] = true;

        if (settings.pelvisDrop) {
            // A leg can always reach at least as far as it actually did
            const float fullLength = bf::length(pos(in, L.knee) - H) + bf::length(pos(in, L.ankle) - pos(in, L.knee));
            const float reach = std::max(fullLength * kReach * 0.998f, bf::length(pos(in, L.ankle) - H));
            const Vec3 w = stepTarget[s] - H;
            if (bf::length(w) > reach) {
                // smallest d >= 0 with |w + d*Y| = reach (body lowered by d)
                const float bY = bf::dot(w, kWorldUp);
                const float disc = bY * bY - (bf::dot(w, w) - reach * reach);
                const float need = disc >= 0.0f ? -bY - std::sqrt(disc) : -bY;
                drop = std::max(drop, need);
            }
        }
    }
    drop = std::clamp(drop, 0.0f, settings.maxPelvisDrop);
    if (drop > 1e-5f) translate(out, kWorldUp * -drop);

    for (int s = 0; s < 2; ++s) {
        const LegJointIndices& L = kLegs[s];
        if (stepActive[s]) {
            const Vec3 H = pos(out, L.hip);
            const float reach = (bf::length(pos(out, L.knee) - H) + bf::length(pos(out, L.ankle) - pos(out, L.knee))) * kReach;
            solveLegIK(out, (Side)s, clampStepTarget(H, stepTarget[s], reach, mIn[s].stepAP, ax), ax.ant);
        } else if (drop > 1e-5f && mIn[s].valid) {
            // The body was lowered for the other leg: keep this foot where it was
            solveLegIK(out, (Side)s, pos(in, L.ankle), ax.ant);
        }
    }

    // --- Joint-angle gains (targets always come from the INPUT pose)
    const BodyAxes axOut = computeAxes(out, settings);
    for (int s = 0; s < 2; ++s) {
        const LimbSettings& ls = settings.limbs[s];
        if (!ls.enabled || !mIn[s].valid || !axOut.valid) continue;
        const Neutral& n = settings.neutral[s];
        const Side side = (Side)s;
        if (ls.mode == LimbMode::JointAngles) {
            if (std::fabs(ls.hipGain - 1.0f) > kGainEps)
                setJointAngle(out, side, LegJoint::Hip, n.hip + ls.hipGain * (mIn[s].hip - n.hip), axOut);
            if (std::fabs(ls.kneeGain - 1.0f) > kGainEps)
                setJointAngle(out, side, LegJoint::Knee, n.knee + ls.kneeGain * (mIn[s].knee - n.knee), axOut);
        }
        if (mIn[s].ankleValid && std::fabs(ls.ankleGain - 1.0f) > kGainEps)
            setJointAngle(out, side, LegJoint::Ankle, n.ankle + ls.ankleGain * (mIn[s].ankle - n.ankle), axOut);
    }

    // --- Ground contact: the lowest foot point stays at its original height
    float groundShift = 0.0f;
    float lowIn = 0.0f, lowOut = 0.0f;
    if (settings.groundContact && lowestFootHeight(in, c, lowIn) && lowestFootHeight(out, c, lowOut)) {
        groundShift = lowIn - lowOut;
        if (std::fabs(groundShift) > 1e-6f) translate(out, kWorldUp * groundShift);
    }

    if (diag) {
        *diag = compare(in, out, settings);
        diag->groundShift = groundShift;
        diag->pelvisDrop = drop;
    }
    return out;
}

BodyDiagnostics compare(const TrackedBody& in, const TrackedBody& out, const Settings& settings) {
    BodyDiagnostics d;
    const BodyAxes axIn = computeAxes(in, settings);
    if (!axIn.valid) return d;
    const BodyAxes axOut = computeAxes(out, settings);
    d.valid = true;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float c = settings.minConfidence;

    for (int s = 0; s < 2; ++s) {
        LimbDiagnostics& ld = d.limbs[s];
        ld.in = measureLeg(in, (Side)s, axIn, c);
        ld.out = measureLeg(out, (Side)s, axOut, c);
        if (!ld.in.valid) continue;

        const LimbSettings& ls = settings.limbs[s];
        const Neutral& n = settings.neutral[s];
        const bool step = ls.enabled && ls.mode == LimbMode::StepLength;
        const float gh = step ? 1.0f : gainOrOne(ls.enabled, ls.hipGain);
        const float gk = step ? 1.0f : gainOrOne(ls.enabled, ls.kneeGain);
        const float ga = gainOrOne(ls.enabled, ls.ankleGain);
        const float gs = step ? ls.stepGain : 1.0f;
        // In step mode hip/knee follow from the IK, so there is no angle target
        ld.target[0] = step ? nan : n.hip + gh * (ld.in.hip - n.hip);
        ld.target[1] = step ? nan : n.knee + gk * (ld.in.knee - n.knee);
        // In step mode a planted foot keeps its orientation, so the ankle angle
        // follows the IK unless an ankle gain is set explicitly
        const bool ankleFree = step && std::fabs(ga - 1.0f) <= kGainEps;
        ld.target[2] = (ld.in.ankleValid && !ankleFree) ? n.ankle + ga * (ld.in.ankle - n.ankle) : nan;
        ld.targetStepAP = n.stepAP + gs * (ld.in.stepAP - n.stepAP);

        if (!ld.out.valid) continue;
        ld.lateralShift = ld.out.stepML - ld.in.stepML;
        const LegJointIndices& L = kLegs[s];
        auto boneErr = [&](int a, int b) {
            return std::fabs(bf::length(pos(out, b) - pos(out, a)) - bf::length(pos(in, b) - pos(in, a)));
        };
        ld.boneLengthError = std::max(boneErr(L.hip, L.knee), boneErr(L.knee, L.ankle));
        if (tracked(in, L.foot, c)) ld.boneLengthError = std::max(ld.boneLengthError, boneErr(L.ankle, L.foot));
        ld.valid = true;
    }
    return d;
}

} // namespace anat
