#include "anatomical_engine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

using bf::Quat;
using bf::Vec3;

namespace anat {

namespace {

const Vec3 kWorldUp(0.0f, 1.0f, 0.0f);
constexpr float kGainEps = 1e-4f;
// The IK never straightens a knee past this (a dead-straight IK leg has no
// defined bend direction); pelvis drop plans for it.
constexpr float kMinIkKnee = 3.0f * 3.14159265358979f / 180.0f;

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

// Tracked = confident AND finite: some sources write NaN for lost keypoints,
// and a NaN must never reach the rotations (it would spread to the chain).
bool tracked(const TrackedBody& b, int j, float minConf) {
    const Joint& p = b.joints[j];
    return p.confidence >= minConf && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

float sanitizeGain(float g) {
    return std::isfinite(g) ? std::clamp(g, 0.0f, 10.0f) : 1.0f;
}

float limitTarget(float target, float input, const JointRange& range, bool enabled) {
    if (!enabled) return target;
    // Never push past the input itself: noisy input outside the range stays as measured
    const float lo = std::min(range.lo, input), hi = std::max(range.hi, input);
    return std::clamp(target, lo, hi);
}

void translate(TrackedBody& b, const Vec3& delta) {
    for (int j = 0; j < JOINT_COUNT; ++j) {
        if (!std::isfinite(b.joints[j].x)) continue;  // leave lost (NaN) keypoints alone
        setPos(b, j, pos(b, j) + delta);
    }
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

// Hip-ankle distance of a leg with thigh L1, shank L2 and knee angle k.
float legSpan(float L1, float L2, float k) {
    return std::sqrt(std::max(0.0f, L1 * L1 + L2 * L2 + 2.0f * L1 * L2 * std::cos(k)));
}

// Step-mode leg solve, in ANGLE space: find hip and knee flexion — each a
// rotation about that joint's own anatomical axis, exactly as in joint-angle
// mode — that put the ankle at the target's forward/back and up/down position
// in the body's sagittal plane. Abduction and rotation are untouched, so the
// leg never leaves its own planes (a positional two-bone IK pinning the
// ankle's sideways position has to tilt a bent leg sideways instead); the
// ankle's sideways position follows from the flexion. Newton's method from
// the person's own angles: the noise in the input is common to the input and
// the target, so near full extension (where the knee angle is hypersensitive
// to the hip-ankle distance) the knee still moves smoothly. If the target is
// out of reach the height is matched first, so the foot stays on the floor and
// the step falls short. The knee never goes straighter than kMinIkKnee (or
// the input, if it was straighter) nor past its range. The foot keeps its
// world orientation: a planted foot doesn't rotate.
struct IkInput {
    float knee = 0.0f;      // measured (signed) knee angle of the input pose
    float rawSpan = 0.0f;   // input hip-ankle distance
};

// Longest span the IK may use: straight to kMinIkKnee, or as straight as the
// input already was (never bend a leg more than the step requires)
float modelSpanLimit(float L1, float L2, const IkInput& ik) {
    return legSpan(L1, L2, std::min(kMinIkKnee, std::fabs(ik.knee)));
}

// How far the hip may be from the target for this leg, in raw-distance terms
float rawReach(const IkInput& ik, float L1, float L2) {
    return ik.rawSpan + (modelSpanLimit(L1, L2, ik) - legSpan(L1, L2, std::fabs(ik.knee)));
}

void solveLegIK(TrackedBody& b, Side side, const Vec3& target, const BodyAxes& axes, const IkInput& ik) {
    const LegJointIndices& L = kLegs[(int)side];
    const Vec3 T0 = pos(b, L.ankle), F0 = pos(b, L.foot);
    const JointPlane hp = jointPlane(b, side, LegJoint::Hip, axes);
    const JointPlane kp = jointPlane(b, side, LegJoint::Knee, axes);
    if (!hp.valid || !kp.valid) return;
    float h = planeAngle(hp, pos(b, L.knee) - hp.centre);
    float k = planeAngle(kp, T0 - kp.centre);
    const float kLo = std::min(kMinIkKnee, ik.knee), kHi = kKneeRange.hi;
    k = std::clamp(k, kLo, kHi);

    auto ankleAt = [&](float hh, float kk) {
        TrackedBody t = b;
        setJointAngle(t, side, LegJoint::Hip, hh, axes);
        setJointAngle(t, side, LegJoint::Knee, kk, axes);
        return pos(t, L.ankle);
    };
    auto residual = [&](const Vec3& T, float& rAnt, float& rUp) {
        rAnt = bf::dot(T - target, axes.ant);
        rUp = bf::dot(T - target, axes.up);
    };

    // Damped least squares (Levenberg-Marquardt) with backtracking: a straight
    // knee makes the Jacobian singular, where plain Newton leaps to a different
    // solution (knee bent 50°+) for one frame. Damping keeps steps small there
    // and a step is only taken if it reduces the error.
    const float eps = 1e-3f, lambda2 = 1e-4f;
    float r0, r1;
    residual(ankleAt(h, k), r0, r1);
    for (int it = 0; it < 15 && r0 * r0 + r1 * r1 > 1e-8f; ++it) {
        float a0, a1, b0, b1;
        residual(ankleAt(h + eps, k), a0, a1);
        residual(ankleAt(h, k + eps), b0, b1);
        const float J00 = (a0 - r0) / eps, J10 = (a1 - r1) / eps;  // d/dh
        const float J01 = (b0 - r0) / eps, J11 = (b1 - r1) / eps;  // d/dk
        // (J^T J + lambda^2 I) d = -J^T r
        const float A00 = J00 * J00 + J10 * J10 + lambda2, A01 = J00 * J01 + J10 * J11;
        const float A11 = J01 * J01 + J11 * J11 + lambda2;
        const float g0 = -(J00 * r0 + J10 * r1), g1 = -(J01 * r0 + J11 * r1);
        const float det = A00 * A11 - A01 * A01;
        if (det < 1e-12f) break;
        float dh = (g0 * A11 - g1 * A01) / det;
        float dk = (g1 * A00 - g0 * A01) / det;
        const float m = std::max(std::fabs(dh), std::fabs(dk));
        if (m > 0.15f) { dh *= 0.15f / m; dk *= 0.15f / m; }
        bool improved = false;
        for (int bt = 0; bt < 6 && !improved; ++bt, dh *= 0.5f, dk *= 0.5f) {
            const float nh = h + dh, nk = std::clamp(k + dk, kLo, kHi);
            float n0, n1;
            residual(ankleAt(nh, nk), n0, n1);
            if (n0 * n0 + n1 * n1 < r0 * r0 + r1 * r1) {
                h = nh;
                k = nk;
                r0 = n0;
                r1 = n1;
                improved = true;
            }
        }
        if (!improved) break;
    }
    // Out of reach (or stuck at a knee limit): match the height with the hip alone
    {
        float r0, r1;
        residual(ankleAt(h, k), r0, r1);
        for (int it = 0; it < 8 && std::fabs(r1) > 5e-4f; ++it) {
            float a0, a1;
            residual(ankleAt(h + eps, k), a0, a1);
            const float d = (a1 - r1) / eps;
            if (std::fabs(d) < 1e-6f) break;
            h += std::clamp(-r1 / d, -0.25f, 0.25f);
            residual(ankleAt(h, k), r0, r1);
        }
    }

    setJointAngle(b, side, LegJoint::Hip, h, axes);
    setJointAngle(b, side, LegJoint::Knee, k, axes);
    setPos(b, L.foot, pos(b, L.ankle) + (F0 - T0));
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

bool lowestFootHeight(const TrackedBody& b, float minConf, const Vec3& up, float& out) {
    bool any = false;
    float low = std::numeric_limits<float>::max();
    for (const auto& L : kLegs) {
        for (int j : {L.ankle, L.foot}) {
            if (!tracked(b, j, minConf)) continue;
            low = std::min(low, bf::dot(pos(b, j), up));
            any = true;
        }
    }
    if (any) out = low;
    return any;
}

Vec3 floorUpOf(const Settings& s) {
    return bf::normalizeOr(s.floorUp, kWorldUp);
}

float gainOrOne(bool enabled, float gain) { return enabled ? gain : 1.0f; }

// Lowest tracked point of one leg's foot (ankle / foot joint) along `up`
bool footLow(const TrackedBody& b, const LegJointIndices& L, float minConf, const Vec3& up, float& h) {
    bool any = false;
    for (int j : {L.ankle, L.foot}) {
        if (!tracked(b, j, minConf)) continue;
        const float v = bf::dot(pos(b, j), up);
        if (!any || v < h) h = v;
        any = true;
    }
    return any;
}

// Keeps the stance foot where it landed by moving the whole output body
// horizontally (perpendicular to `up`). Called with the output after all
// other corrections; returns the planted side (-1: none). With planting off
// any remaining offset eases back to zero at 1 m/s.
int plantStanceFoot(const TrackedBody& in, TrackedBody& out, const Settings& settings, const Vec3& up,
                    TemporalState& st, double time) {
    const float c = settings.minConfidence;
    const float dt = st.valid ? (float)std::max(0.0, time - st.time) : 0.0f;

    // Is the person standing still? (input pelvis horizontal speed)
    const int pelvis = (int)CanonicalJoint::PELVIS;
    bool still = false;
    if (tracked(in, pelvis, c)) {
        const Vec3 p = pos(in, pelvis);
        if (st.havePelvis && dt > 0.0f) still = bf::length(bf::rejectFrom(p - st.lastPelvis, up)) / dt < 0.15f;
        st.lastPelvis = p;
        st.havePelvis = true;
    }
    st.stillTime = still ? st.stillTime + dt : 0.0f;

    int stance = -1;
    if (settings.plantStanceFoot) {
        // The stance foot is the one the PERSON stands on: low AND still in
        // the input (the modified pose can't say — scaling changes which foot
        // is lower). Height alone switches a frame or two early, while the
        // landing foot is still moving, which would drag the avatar with it.
        float h[2] = {0, 0}, speed[2] = {0, 0};
        bool ok[2] = {false, false};
        for (int s = 0; s < 2; ++s) {
            const LegJointIndices& L = kLegs[s];
            ok[s] = footLow(in, L, c, up, h[s]);
            const int j = tracked(in, L.ankle, c) ? L.ankle : L.foot;
            if (!ok[s]) { st.lastFootValid[s] = false; continue; }
            const Vec3 p = bf::rejectFrom(pos(in, j), up);
            speed[s] = (st.lastFootValid[s] && dt > 0.0f) ? bf::length(p - st.lastFoot[s]) / dt : 0.0f;
            st.lastFoot[s] = p;
            st.lastFootValid[s] = true;
        }
        if (ok[0] && ok[1]) {
            const float hmin = std::min(h[0], h[1]);
            float cost[2];
            for (int s = 0; s < 2; ++s) cost[s] = (h[s] - hmin) / 0.02f + speed[s] / 0.3f;
            stance = cost[0] <= cost[1] ? 0 : 1;
            // hysteresis: keep the planted foot unless the other is clearly the better contact
            if (st.stanceSide >= 0 && stance != st.stanceSide && cost[st.stanceSide] - cost[stance] < 0.5f)
                stance = st.stanceSide;
        } else if (ok[0] || ok[1]) {
            stance = ok[0] ? 0 : 1;
        }
        if (stance >= 0) {
            const LegJointIndices& L = kLegs[stance];
            const int j = tracked(out, L.ankle, c) ? L.ankle : L.foot;
            const Vec3 contact = bf::rejectFrom(pos(out, j), up);
            if (!st.plantValid || stance != st.stanceSide) {
                // new contact: plant where the foot is now (with the current offset)
                st.plantPoint = contact + st.rootOffset;
                st.plantValid = true;
                st.stanceSide = stance;
            }
            Vec3 offset = st.plantPoint - contact;
            // standing still: drift back toward the person
            if (st.stillTime > 0.5f) {
                const float len = bf::length(offset), stepLen = settings.rootRecenterSpeed * dt;
                offset = len <= stepLen ? Vec3() : offset * ((len - stepLen) / len);
                st.plantPoint = contact + offset;
            }
            st.rootOffset = offset;
        }
    } else {
        st.plantValid = false;
        st.stanceSide = -1;
        const float len = bf::length(st.rootOffset), stepLen = 1.0f * dt;
        st.rootOffset = (len <= stepLen || dt <= 0.0f) ? (dt <= 0.0f ? st.rootOffset : Vec3())
                                                       : st.rootOffset * ((len - stepLen) / len);
    }
    if (bf::length(st.rootOffset) > 1e-6f) translate(out, st.rootOffset);
    return stance;
}

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

    const Vec3 vertical = floorUpOf(settings);
    Vec3 upRaw = vertical;
    if (settings.hipReference == HipReference::Pelvis) {
        const int navel = (int)CanonicalJoint::SPINE_NAVEL, chest = (int)CanonicalJoint::SPINE_CHEST;
        if (tracked(body, navel, c)) upRaw = pos(body, navel) - ax.origin;
        else if (tracked(body, chest, c)) upRaw = pos(body, chest) - ax.origin;
    }
    if (!bf::tryNormalize(bf::rejectFrom(upRaw, ax.ml), ax.up) &&
        !bf::tryNormalize(bf::rejectFrom(vertical, ax.ml), ax.up))
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

namespace {

// Foot-direction vote for one body: sum of cosines (each foot in [-1, 1]) of
// the horizontal foot vector against ml × up. Returns the number of feet used.
int footVote(const TrackedBody& b, float minConf, double& vote) {
    Settings s;
    s.forwardSign = 1;
    s.minConfidence = minConf;
    const BodyAxes ax = computeAxes(b, s);
    vote = 0.0;
    if (!ax.valid) return 0;
    int used = 0;
    for (const auto& L : kLegs) {
        if (!tracked(b, L.ankle, minConf) || !tracked(b, L.foot, minConf)) continue;
        Vec3 f = bf::rejectFrom(pos(b, L.foot) - pos(b, L.ankle), ax.up);
        if (!ax.planar) f = bf::rejectFrom(f, ax.ml);
        const float len = bf::length(f);
        if (len < 0.02f) continue;  // foot seen end-on — no information
        vote += bf::dot(f, ax.ant) / len;
        ++used;
    }
    return used;
}

} // namespace

std::vector<int> forwardSignsPerFrame(const std::vector<const TrackedBody*>& bodies, int fallback,
                                      int window, float minConf) {
    const int n = (int)bodies.size();
    std::vector<double> vote(n, 0.0);
    std::vector<int> used(n, 0);
    for (int i = 0; i < n; ++i)
        if (bodies[i]) used[i] = footVote(*bodies[i], minConf, vote[i]);

    // windowed sums via prefix sums
    std::vector<double> pv(n + 1, 0.0);
    std::vector<int> pu(n + 1, 0);
    for (int i = 0; i < n; ++i) {
        pv[i + 1] = pv[i] + vote[i];
        pu[i + 1] = pu[i] + used[i];
    }
    const int half = std::max(1, window / 2);
    std::vector<int> sign(n, 0);
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - half), b = std::min(n, i + half + 1);
        const double v = pv[b] - pv[a];
        const int u = pu[b] - pu[a];
        if (u >= 3 && std::fabs(v) / u >= 0.3) sign[i] = v > 0 ? 1 : -1;
    }
    // undecided frames take the nearest decided sign (forward, then backward fill)
    int last = 0;
    for (int i = 0; i < n; ++i) {
        if (sign[i] != 0) last = sign[i];
        else if (last != 0) sign[i] = last;
    }
    last = 0;
    for (int i = n - 1; i >= 0; --i) {
        if (sign[i] != 0) last = sign[i];
        else sign[i] = last != 0 ? last : (fallback >= 0 ? 1 : -1);
    }
    return sign;
}

bool lowestFootPoint(const TrackedBody& b, Vec3& out, float minConf) {
    bool any = false;
    for (const auto& L : kLegs) {
        for (int j : {L.ankle, L.foot}) {
            if (!tracked(b, j, minConf)) continue;
            const Vec3 p = pos(b, j);
            if (!any || p.y < out.y) { out = p; any = true; }
        }
    }
    return any;
}

void swapLeftRight(TrackedBody& b) {
    static const int pairs[][2] = {{4, 11}, {5, 12}, {6, 13}, {7, 14}, {8, 15}, {9, 16}, {10, 17},
                                   {18, 22}, {19, 23}, {20, 24}, {21, 25}, {28, 30}, {29, 31}};
    for (const auto& pr : pairs) {
        std::swap(b.joints[pr[0]], b.joints[pr[1]]);
        std::swap(b.joints2D[pr[0]], b.joints2D[pr[1]]);
    }
}

double forwardVote(const TrackedBody& b, int& used, float minConf) {
    double v = 0.0;
    used = footVote(b, minConf, v);
    return v;
}

FloorPlane estimateFloor(const std::vector<const TrackedBody*>& bodies, float minConf) {
    std::vector<Vec3> pts;
    pts.reserve(bodies.size());
    for (const TrackedBody* b : bodies) {
        Vec3 p;
        if (b && lowestFootPoint(*b, p, minConf)) pts.push_back(p);
    }
    return estimateFloorFromPoints(pts);
}

FloorPlane estimateFloorFromPoints(const std::vector<Vec3>& lowestPoints) {
    // Per-frame lowest foot point. Its height is NOT always the floor: at heel
    // strike the lowest tracked point is the ankle (~7 cm up), at push-off it is
    // the toe on the floor, and on a treadmill those happen at fixed front/back
    // positions, which looks exactly like a tilt. So a tilt is only fitted when
    // the subject travelled far enough (>= kMinTravel) that every patch of floor
    // saw both kinds of contact, and it is fitted through the LOW envelope of
    // 20 cm floor cells. Otherwise camera Y is assumed (level camera).
    constexpr double kCell = 0.2, kMinTravel = 1.5;
    FloorPlane fp;
    struct P { double x, y, z; };
    std::vector<P> pts;
    pts.reserve(lowestPoints.size());
    for (const Vec3& p : lowestPoints)
        if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) pts.push_back({p.x, p.y, p.z});
    if (pts.size() < 30) return fp;

    // Low envelope per floor cell: 10th percentile of the heights in the cell
    std::map<std::pair<int, int>, std::vector<P>> cells;
    for (const P& p : pts) cells[{(int)std::floor(p.x / kCell), (int)std::floor(p.z / kCell)}].push_back(p);
    std::vector<P> env;
    for (auto& [key, cp] : cells) {
        if (cp.size() < 5) continue;
        std::vector<double> ys;
        double mx = 0, mz = 0;
        for (const P& p : cp) { ys.push_back(p.y); mx += p.x; mz += p.z; }
        std::sort(ys.begin(), ys.end());
        env.push_back({mx / cp.size(), ys[ys.size() / 10], mz / cp.size()});
    }
    double minX = 1e9, maxX = -1e9, minZ = 1e9, maxZ = -1e9;
    for (const P& p : env) {
        minX = std::min(minX, p.x); maxX = std::max(maxX, p.x);
        minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z);
    }
    const bool fitX = env.size() >= 4 && (maxX - minX) >= kMinTravel;
    const bool fitZ = env.size() >= 4 && (maxZ - minZ) >= kMinTravel;

    double a = 0, bz = 0;
    if (fitX || fitZ) {
        std::vector<char> keep(env.size(), 1);
        for (int iter = 0; iter < 3; ++iter) {
            // least squares y = a x + b z + c over kept cells (centred for conditioning)
            double cx = 0, cz = 0, cy = 0, N = 0;
            for (size_t i = 0; i < env.size(); ++i)
                if (keep[i]) { cx += env[i].x; cz += env[i].z; cy += env[i].y; N += 1; }
            if (N < 4) break;
            cx /= N; cz /= N; cy /= N;
            double Sxx = 0, Sxz = 0, Szz = 0, Sxy = 0, Szy = 0;
            for (size_t i = 0; i < env.size(); ++i) {
                if (!keep[i]) continue;
                const double x = fitX ? env[i].x - cx : 0.0, z = fitZ ? env[i].z - cz : 0.0, y = env[i].y - cy;
                Sxx += x * x; Sxz += x * z; Szz += z * z; Sxy += x * y; Szy += z * y;
            }
            const double det = Sxx * Szz - Sxz * Sxz;
            if (fitX && fitZ && std::fabs(det) > 1e-12) {
                a = (Sxy * Szz - Szy * Sxz) / det;
                bz = (Szy * Sxx - Sxy * Sxz) / det;
            } else if (fitZ && Szz > 1e-12) {
                a = 0; bz = Szy / Szz;
            } else if (fitX && Sxx > 1e-12) {
                a = Sxy / Sxx; bz = 0;
            }
            // drop cells far off the plane (e.g. a cell only ever seen at heel strike) and refit
            bool changed = false;
            for (size_t i = 0; i < env.size(); ++i) {
                const double r = env[i].y - cy - a * (env[i].x - cx) - bz * (env[i].z - cz);
                const char k = std::fabs(r) <= 0.03 ? 1 : 0;
                if (k != keep[i]) { keep[i] = k; changed = true; }
            }
            if (!changed) break;
        }
    }

    Vec3 n((float)-a, 1.0f, (float)-bz);
    fp.up = n / bf::length(n);
    fp.tiltDeg = std::acos(std::clamp(fp.up.y, -1.0f, 1.0f)) * 57.2957795f;
    if (fp.tiltDeg > 35.0f) {  // implausible: not a floor
        fp.up = kWorldUp;
        fp.tiltDeg = 0.0f;
    }
    // floor height along `up`: a low percentile of contact heights
    std::vector<float> h;
    for (const P& p : pts) h.push_back(bf::dot(Vec3((float)p.x, (float)p.y, (float)p.z), fp.up));
    std::sort(h.begin(), h.end());
    fp.height = h[h.size() / 50];
    fp.valid = true;
    return fp;
}

int fillTrackingGaps(std::vector<TrackedBody*>& frames, const std::vector<double>& times,
                     double maxGapSeconds, float minConf, float filledConfidence) {
    const int n = (int)std::min(frames.size(), times.size());
    int filled = 0;
    for (int j = 0; j < JOINT_COUNT; ++j) {
        int lastGood = -1;  // index of the last frame where joint j was tracked
        for (int i = 0; i < n; ++i) {
            if (!frames[i]) { lastGood = -1; continue; }  // subject absent: no bridging
            if (tracked(*frames[i], j, minConf)) {
                if (lastGood >= 0 && i - lastGood > 1 && times[i] - times[lastGood] <= maxGapSeconds) {
                    const TrackedBody& a = *frames[lastGood];
                    const TrackedBody& b = *frames[i];
                    const double span = times[i] - times[lastGood];
                    for (int k = lastGood + 1; k < i; ++k) {
                        const float f = span > 0 ? (float)((times[k] - times[lastGood]) / span) : 0.5f;
                        Joint& jt = frames[k]->joints[j];
                        jt.x = a.joints[j].x + (b.joints[j].x - a.joints[j].x) * f;
                        jt.y = a.joints[j].y + (b.joints[j].y - a.joints[j].y) * f;
                        jt.z = a.joints[j].z + (b.joints[j].z - a.joints[j].z) * f;
                        jt.confidence = std::min({filledConfidence, a.joints[j].confidence, b.joints[j].confidence});
                        frames[k]->joints2D[j].valid = false;
                        ++filled;
                    }
                }
                lastGood = i;
            }
        }
    }
    return filled;
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

TrackedBody apply(const TrackedBody& in, const Settings& settingsIn, BodyDiagnostics* diag,
                  TemporalState* state, double time) {
    Settings settings = settingsIn;
    for (auto& ls : settings.limbs) {
        ls.hipGain = sanitizeGain(ls.hipGain);
        ls.kneeGain = sanitizeGain(ls.kneeGain);
        ls.ankleGain = sanitizeGain(ls.ankleGain);
        ls.stepGain = sanitizeGain(ls.stepGain);
    }
    const Vec3 up = floorUpOf(settings);

    // Temporal limit on vertical corrections (needs the previous frame of the same body)
    float maxStep = -1.0f;  // < 0: unlimited
    if (state) {
        const double dt = time - state->time;
        const bool continuous = state->valid && state->bodyId == in.id && dt > 0.0 && dt <= 0.25;
        if (!continuous) {
            const TemporalState fresh;
            state->valid = false;
            state->rootOffset = fresh.rootOffset;  // a gap or another body: start over at the person
            state->plantValid = false;
            state->stanceSide = -1;
            state->havePelvis = false;
            state->stillTime = 0.0f;
            state->lastFootValid[0] = state->lastFootValid[1] = false;
        } else if (settings.maxVerticalSpeed > 0.0f) {
            maxStep = settings.maxVerticalSpeed * (float)dt;
        }
    }
    auto limitStep = [&](float value, float previous) {
        if (maxStep < 0.0f || !state || !state->valid) return value;
        return std::clamp(value, previous - maxStep, previous + maxStep);
    };
    auto remember = [&](float drop, float ground) {
        if (!state) return;
        state->valid = true;
        state->bodyId = in.id;
        state->time = time;
        state->pelvisDrop = drop;
        state->groundShift = ground;
    };

    TrackedBody out = in;
    const BodyAxes ax = computeAxes(in, settings);
    const bool anyEnabled = settings.limbs[0].enabled || settings.limbs[1].enabled;
    if (!ax.valid || !anyEnabled) {
        // keep the state flowing toward zero so a re-enabled leg eases back in
        const float d0 = limitStep(0.0f, state ? state->pelvisDrop : 0.0f);
        const float g0 = limitStep(0.0f, state ? state->groundShift : 0.0f);
        const Vec3 up0 = floorUpOf(settings);
        if (state && std::fabs(d0 - g0) > 1e-6f) translate(out, up0 * (g0 - d0));
        int stance0 = -1;
        if (state) {
            Settings off = settings;
            off.plantStanceFoot = false;  // nothing modified: just ease any offset back to zero
            stance0 = plantStanceFoot(in, out, off, up0, *state, time);
        }
        remember(d0, g0);
        if (diag) {
            *diag = compare(in, out, settings);
            diag->pelvisDrop = d0;
            diag->groundShift = g0;
            diag->rootOffset = state ? state->rootOffset : Vec3();
            diag->stanceSide = stance0;
        }
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
        if (std::fabs(ls.stepGain - 1.0f) <= kGainEps) continue;  // nothing to do
        const LegJointIndices& L = kLegs[s];
        const Neutral& n = settings.neutral[s];
        const Vec3 H = pos(in, L.hip);
        const float ap = n.stepAP + ls.stepGain * (mIn[s].stepAP - n.stepAP);
        stepTarget[s] = H + ax.ml * mIn[s].stepML + ax.up * mIn[s].stepUp + ax.ant * ap;
        stepActive[s] = true;

        if (settings.pelvisDrop) {
            const float L1 = bf::length(pos(in, L.knee) - H), L2 = bf::length(pos(in, L.ankle) - pos(in, L.knee));
            IkInput ik{mIn[s].knee, bf::length(pos(in, L.ankle) - H)};
            // a leg can always reach at least as far as it actually did
            const float reach = std::max(rawReach(ik, L1, L2), ik.rawSpan);
            const Vec3 w = stepTarget[s] - H;
            if (bf::length(w) > reach) {
                // smallest d >= 0 with |w + d*up| = reach (body lowered by d)
                const float bY = bf::dot(w, up);
                const float disc = bY * bY - (bf::dot(w, w) - reach * reach);
                const float need = disc >= 0.0f ? -bY - std::sqrt(disc) : -bY;
                drop = std::max(drop, need);
            }
        }
    }
    drop = std::clamp(drop, 0.0f, std::max(0.0f, settings.maxPelvisDrop));
    if (state && state->valid) drop = std::max(0.0f, limitStep(drop, state->pelvisDrop));
    if (drop > 1e-5f) translate(out, up * -drop);

    for (int s = 0; s < 2; ++s) {
        const LegJointIndices& L = kLegs[s];
        if (!mIn[s].valid || (!stepActive[s] && drop <= 1e-5f)) continue;
        const Vec3 Hin = pos(in, L.hip);
        const float L1 = bf::length(pos(in, L.knee) - Hin), L2 = bf::length(pos(in, L.ankle) - pos(in, L.knee));
        const IkInput ik{mIn[s].knee, bf::length(pos(in, L.ankle) - Hin)};
        if (stepActive[s]) {
            const Vec3 H = pos(out, L.hip);
            const float reach = std::max(rawReach(ik, L1, L2), ik.rawSpan);
            solveLegIK(out, (Side)s, clampStepTarget(H, stepTarget[s], reach, mIn[s].stepAP, ax), ax, ik);
        } else {
            // The body was lowered for the other leg: keep this foot where it was
            solveLegIK(out, (Side)s, pos(in, L.ankle), ax, ik);
        }
    }

    // --- Joint-angle gains (targets always come from the INPUT pose, then are
    //     kept inside physiological ranges)
    const BodyAxes axOut = computeAxes(out, settings);
    const bool lim = settings.limitToJointRanges;
    for (int s = 0; s < 2; ++s) {
        const LimbSettings& ls = settings.limbs[s];
        if (!ls.enabled || !mIn[s].valid || !axOut.valid) continue;
        const Neutral& n = settings.neutral[s];
        const Side side = (Side)s;
        const LegMeasure& m = mIn[s];
        if (ls.mode == LimbMode::JointAngles) {
            if (std::fabs(ls.hipGain - 1.0f) > kGainEps)
                setJointAngle(out, side, LegJoint::Hip,
                              limitTarget(n.hip + ls.hipGain * (m.hip - n.hip), m.hip, kHipRange, lim), axOut);
            if (std::fabs(ls.kneeGain - 1.0f) > kGainEps)
                setJointAngle(out, side, LegJoint::Knee,
                              limitTarget(n.knee + ls.kneeGain * (m.knee - n.knee), m.knee, kKneeRange, lim), axOut);
        }
        if (m.ankleValid && std::fabs(ls.ankleGain - 1.0f) > kGainEps)
            setJointAngle(out, side, LegJoint::Ankle,
                          limitTarget(n.ankle + ls.ankleGain * (m.ankle - n.ankle), m.ankle, kAnkleRange, lim), axOut);
    }

    // --- Ground contact: the lowest foot point stays at its original height above the floor
    float groundShift = 0.0f;
    float lowIn = 0.0f, lowOut = 0.0f;
    if (settings.groundContact && lowestFootHeight(in, c, up, lowIn) && lowestFootHeight(out, c, up, lowOut)) {
        groundShift = lowIn - lowOut;
    } else if (settings.groundContact && state && state->valid) {
        groundShift = state->groundShift;  // no foot tracked: hold the last correction
    }
    if (state && state->valid) groundShift = limitStep(groundShift, state->groundShift);
    if (std::fabs(groundShift) > 1e-6f) translate(out, up * groundShift);

    // --- Stance-foot planting (overground)
    int stance = -1;
    if (state) stance = plantStanceFoot(in, out, settings, up, *state, time);

    remember(drop, groundShift);

    if (diag) {
        *diag = compare(in, out, settings);
        diag->groundShift = groundShift;
        diag->pelvisDrop = drop;
        diag->rootOffset = state ? state->rootOffset : Vec3();
        diag->stanceSide = stance;
    }
    return out;
}

namespace {

// Component of each segment along its parent's flexion axis: thigh vs pelvis
// ml, shank vs thigh ml, foot vs shank ml. Unchanged by any flexion.
struct PlaneComponents {
    float thigh = 0, shank = 0, foot = 0;
    float L1 = 0, L2 = 0, Lf = 0;
    bool footOk = false;
};

bool planeComponents(const TrackedBody& b, Side side, const BodyAxes& ax, float minConf, PlaneComponents& pc) {
    const LegJointIndices& L = kLegs[(int)side];
    if (!ax.valid || !tracked(b, L.hip, minConf) || !tracked(b, L.knee, minConf) || !tracked(b, L.ankle, minConf))
        return false;
    const Vec3 H = pos(b, L.hip), K = pos(b, L.knee), T = pos(b, L.ankle);
    Vec3 dT, dS;
    if (!bf::tryNormalize(K - H, dT) || !bf::tryNormalize(T - K, dS)) return false;
    const SegmentFrame thigh = segmentFrame(dT, ax.ml, ax.forwardSign);
    const SegmentFrame shank = segmentFrame(dS, thigh.m, ax.forwardSign);
    pc.thigh = bf::dot(dT, ax.ml);
    pc.shank = bf::dot(dS, thigh.m);
    pc.L1 = bf::length(K - H);
    pc.L2 = bf::length(T - K);
    pc.footOk = false;
    if (tracked(b, L.foot, minConf)) {
        Vec3 dF;
        if (bf::tryNormalize(pos(b, L.foot) - T, dF)) {
            pc.foot = bf::dot(dF, shank.m);
            pc.Lf = bf::length(pos(b, L.foot) - T);
            pc.footOk = true;
        }
    }
    return true;
}

float offPlaneDistance(const TrackedBody& in, const TrackedBody& out, Side side, const BodyAxes& axIn,
                       const BodyAxes& axOut, float minConf) {
    PlaneComponents a, b;
    if (!planeComponents(in, side, axIn, minConf, a) || !planeComponents(out, side, axOut, minConf, b)) return 0.0f;
    float d = a.L1 * std::fabs(b.thigh - a.thigh) + a.L2 * std::fabs(b.shank - a.shank);
    if (a.footOk && b.footOk) d += a.Lf * std::fabs(b.foot - a.foot);
    return d;
}

} // namespace

BodyDiagnostics compare(const TrackedBody& in, const TrackedBody& out, const Settings& settings) {
    BodyDiagnostics d;
    const bool lim = settings.limitToJointRanges;
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
        const float gh = step ? 1.0f : gainOrOne(ls.enabled, sanitizeGain(ls.hipGain));
        const float gk = step ? 1.0f : gainOrOne(ls.enabled, sanitizeGain(ls.kneeGain));
        const float ga = gainOrOne(ls.enabled, sanitizeGain(ls.ankleGain));
        const float gs = step ? sanitizeGain(ls.stepGain) : 1.0f;
        // In step mode hip/knee follow from the IK, so there is no angle target
        ld.target[0] = step ? nan : limitTarget(n.hip + gh * (ld.in.hip - n.hip), ld.in.hip, kHipRange, lim);
        ld.target[1] = step ? nan : limitTarget(n.knee + gk * (ld.in.knee - n.knee), ld.in.knee, kKneeRange, lim);
        // In step mode a planted foot keeps its orientation, so the ankle angle
        // follows the IK unless an ankle gain is set explicitly
        const bool ankleFree = step && std::fabs(ga - 1.0f) <= kGainEps;
        ld.target[2] = (ld.in.ankleValid && !ankleFree)
                           ? limitTarget(n.ankle + ga * (ld.in.ankle - n.ankle), ld.in.ankle, kAnkleRange, lim)
                           : nan;
        ld.targetStepAP = n.stepAP + gs * (ld.in.stepAP - n.stepAP);

        if (!ld.out.valid) continue;
        ld.lateralShift = ld.out.stepML - ld.in.stepML;
        ld.offPlane = offPlaneDistance(in, out, (Side)s, axIn, axOut, c);
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
