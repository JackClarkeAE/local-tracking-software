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

// Two-bone IK, driven from the MEASURED knee angle. Near full extension the
// knee angle is hypersensitive to the hip-ankle distance, so deriving the knee
// from raw (noisy) joint positions turns a few mm of tracking jitter into
// ±10° of knee flicker. Instead the knee starts from its measured angle and
// only changes by what the change in hip-ankle distance requires; the noise
// is common to both distances and cancels. The ankle lands on the line from
// the hip to `target`, within the input's own jitter of the target.
//
// The knee bends toward the thigh's anatomical anterior axis (never toward
// the noisy offset of a straight knee from the hip-ankle line), or away from
// it if the input was clearly hyperextended. The foot keeps its orientation
// in the world: IK is only used where the foot should stay planted, so the
// ankle angle absorbs the change, as in a real longer stride.
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
    const Vec3 H = pos(b, L.hip), K = pos(b, L.knee), T = pos(b, L.ankle), F = pos(b, L.foot);
    const float L1 = bf::length(K - H), L2 = bf::length(T - K);
    if (L1 < 1e-3f || L2 < 1e-3f) return;

    Vec3 e;
    if (!bf::tryNormalize(target - H, e)) return;
    const float delta = bf::length(target - H) - ik.rawSpan;  // what the step asks for
    const float span = std::clamp(legSpan(L1, L2, std::fabs(ik.knee)) + delta,
                                  std::fabs(L1 - L2) + 1e-3f, modelSpanLimit(L1, L2, ik));
    const bool hyper = ik.knee < -2.0f * kDegToRad;

    // bend direction: the thigh's anterior axis, perpendicular to the hip-ankle line
    Vec3 thighDir;
    if (!bf::tryNormalize(K - H, thighDir)) thighDir = e;
    const Vec3 thighAnt = segmentFrame(thighDir, axes.ml, axes.forwardSign).a;
    Vec3 pole = bf::rejectFrom(thighAnt, e);
    if (!bf::tryNormalize(pole, pole)) pole = bf::normalizeOr(bf::rejectFrom(axes.ant, e), Vec3(0, 0, 1));
    if (hyper) pole = -pole;

    const float a = (L1 * L1 - L2 * L2 + span * span) / (2.0f * span);
    const float h = std::sqrt(std::max(0.0f, L1 * L1 - a * a));
    const Vec3 newK = H + e * a + pole * h;
    const Vec3 newT = H + e * span;

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

FloorPlane estimateFloor(const std::vector<const TrackedBody*>& bodies, float minConf) {
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
    pts.reserve(bodies.size());
    for (const TrackedBody* b : bodies) {
        if (!b) continue;
        bool any = false;
        Vec3 low;
        for (const auto& L : kLegs) {
            for (int j : {L.ankle, L.foot}) {
                if (!tracked(*b, j, minConf)) continue;
                const Vec3 p = pos(*b, j);
                if (!any || p.y < low.y) { low = p; any = true; }
            }
        }
        if (any) pts.push_back({low.x, low.y, low.z});
    }
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
        if (state->valid && state->bodyId == in.id && dt > 0.0 && dt <= 0.25 && settings.maxVerticalSpeed > 0.0f)
            maxStep = settings.maxVerticalSpeed * (float)dt;
        else
            state->valid = false;
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
        remember(limitStep(0.0f, state ? state->pelvisDrop : 0.0f), limitStep(0.0f, state ? state->groundShift : 0.0f));
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
    remember(drop, groundShift);

    if (diag) {
        *diag = compare(in, out, settings);
        diag->groundShift = groundShift;
        diag->pelvisDrop = drop;
    }
    return out;
}

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
