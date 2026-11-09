#include "anatomical_live.h"

#include <algorithm>
#include <cmath>

using bf::Vec3;

namespace anat {

namespace {

bool usable(const Joint& j, float minConf) {
    return j.confidence >= minConf && std::isfinite(j.x) && std::isfinite(j.y) && std::isfinite(j.z);
}

Vec3 at(const TrackedBody& b, int j) { return {b.joints[j].x, b.joints[j].y, b.joints[j].z}; }

// Parent of each joint along the skeleton (from the bone list, which runs
// proximal → distal), and an order that visits parents before children.
struct Skeleton {
    std::array<int, JOINT_COUNT> parent{};
    std::vector<int> order;
    Skeleton() {
        parent.fill(-1);
        std::array<bool, JOINT_COUNT> listed{};
        order.push_back((int)CanonicalJoint::PELVIS);
        listed[(int)CanonicalJoint::PELVIS] = true;
        for (const auto& [a, b] : getBoneConnections()) {
            if (b < 0 || b >= JOINT_COUNT || listed[b]) continue;
            parent[b] = a;
            order.push_back(b);
            listed[b] = true;
        }
        for (int j = 0; j < JOINT_COUNT; ++j)
            if (!listed[j]) order.push_back(j);
    }
};

const Skeleton& skeleton() {
    static const Skeleton s;
    return s;
}

int signOf(double v) { return v >= 0.0 ? 1 : -1; }

} // namespace

void LiveProcessor::reset() {
    const Options keep = options_;
    *this = LiveProcessor{};
    options_ = keep;
}

void LiveProcessor::holdGaps(TrackedBody& b, double time) {
    const float c = options_.minConfidence;
    std::array<bool, JOINT_COUNT> avail{};
    for (int j : skeleton().order) {
        const int p = skeleton().parent[j];
        Joint& jt = b.joints[j];
        if (usable(jt, c)) {
            lastPos_[j] = at(b, j);
            lastSeen_[j] = time;
            seen_[j] = true;
            if (p >= 0 && avail[p]) {
                lastOffset_[j] = lastPos_[j] - at(b, p);
                offsetValid_[j] = true;
            }
            avail[j] = true;
            continue;
        }
        if (!seen_[j] || time - lastSeen_[j] > options_.maxGapSeconds) continue;
        // Dropped: ride along with the parent at the last known offset, or
        // stay where it was if the parent is gone too
        const Vec3 held = (p >= 0 && avail[p] && offsetValid_[j]) ? at(b, p) + lastOffset_[j] : lastPos_[j];
        jt.x = held.x;
        jt.y = held.y;
        jt.z = held.z;
        jt.confidence = std::max(c, 0.34f);
        b.joints2D[j].valid = false;
        avail[j] = true;
        ++status_.jointsHeld;
    }
}

void LiveProcessor::updateForward(TrackedBody& b, double time, double dt) {
    int used = 0;
    double vote = forwardVote(b, used, options_.minConfidence);
    const double decay = dt > 0.0 ? std::exp(-dt / 0.4) : 1.0;  // ~0.5 s recent window
    recentVote_ = recentVote_ * decay + vote;
    recentUsed_ = recentUsed_ * decay + used;
    const bool strong = recentUsed_ >= 3.0 && std::fabs(recentVote_) / recentUsed_ >= 0.3;
    const int recentSign = signOf(recentVote_);
    const int hint = options_.forwardHint >= 0 ? (options_.forwardHint == 0 ? 0 : 1) : -1;

    if (status_.planar) {
        // 2D: the subject can face either way in the image; follow them with hysteresis
        status_.swapped = false;
        if (frameSign_ == 0) frameSign_ = strong ? recentSign : (hint != 0 ? hint : 1);
        if (strong && recentSign != frameSign_) {
            if (disagreeSince_ < 0) disagreeSince_ = time;
            if (time - disagreeSince_ >= 0.3) {
                frameSign_ = recentSign;
                disagreeSince_ = -1;
            }
        } else {
            disagreeSince_ = -1;
        }
        status_.forwardSign = frameSign_;
        status_.forwardLearned = strong;
        return;
    }

    // 3D: handedness is a property of the camera's coordinates. Known from the
    // camera type (hint) or learned from the feet; frames whose recent votes
    // persistently disagree have had left/right swapped by the tracker.
    const bool learned = totalUsed_ >= 10 && std::fabs(totalVote_) / totalUsed_ >= 0.2;
    const int sign = hint != 0 ? hint : (learned ? signOf(totalVote_) : 1);
    status_.forwardLearned = hint != 0 || learned;
    if (options_.fixLeftRightSwaps && status_.forwardLearned && strong) {
        if (recentSign != sign) {
            agreeSince_ = -1;
            if (disagreeSince_ < 0) disagreeSince_ = time;
            if (time - disagreeSince_ >= 0.3) status_.swapped = true;
        } else {
            disagreeSince_ = -1;
            if (agreeSince_ < 0) agreeSince_ = time;
            if (time - agreeSince_ >= 0.3) status_.swapped = false;
        }
    }
    if (!options_.fixLeftRightSwaps) status_.swapped = false;
    if (status_.swapped) {
        swapLeftRight(b);
        vote = -vote;
    }
    if (used) {
        totalVote_ += vote;  // learn from corrected votes only
        totalUsed_ += used;
    }
    status_.forwardSign = sign;
}

void LiveProcessor::updateFloor(const TrackedBody& b, double time) {
    Vec3 p;
    if (lowestFootPoint(b, p, options_.minConfidence)) {
        lowPoints_.push_back(p);
        if (lowPoints_.size() > 6000) lowPoints_.pop_front();
    }
    // refit every second at first, then every 5 s (each fit bins up to 6000 points)
    const double interval = lowPoints_.size() < 900 ? 1.0 : 5.0;
    if (time - lastFloorFit_ < interval || lowPoints_.size() < 30) return;
    lastFloorFit_ = time;
    if (options_.learnFloor) {
        status_.floor = estimateFloorFromPoints(std::vector<Vec3>(lowPoints_.begin(), lowPoints_.end()));
    } else {
        std::vector<float> ys;
        for (const Vec3& q : lowPoints_) ys.push_back(q.y);
        std::sort(ys.begin(), ys.end());
        status_.floor = FloorPlane{};
        status_.floor.valid = true;
        status_.floor.height = ys[ys.size() / 50];
    }
}

void LiveProcessor::updateLocomotion(const TrackedBody& b, double time) {
    if (options_.locomotion != Locomotion::Auto) {
        status_.overground = options_.locomotion == Locomotion::Overground;
        return;
    }
    const Joint& p = b.joints[(int)CanonicalJoint::PELVIS];
    if (usable(p, options_.minConfidence)) {
        const Vec3 up = status_.floor.valid ? status_.floor.up : Vec3(0, 1, 0);
        pelvisTrack_.emplace_back(time, bf::rejectFrom(at(b, (int)CanonicalJoint::PELVIS), up));
    }
    while (!pelvisTrack_.empty() && pelvisTrack_.front().first < time - 4.0) pelvisTrack_.pop_front();
    if (pelvisTrack_.size() < 2) return;
    const double span = pelvisTrack_.back().first - pelvisTrack_.front().first;
    const float travel = bf::length(pelvisTrack_.back().second - pelvisTrack_.front().second);
    if (travel > 1.2f) status_.overground = true;
    else if (travel < 0.4f && span >= 3.0) status_.overground = false;
}

TrackedBody LiveProcessor::process(const TrackedBody& in, double time, const Settings& settings,
                                   BodyDiagnostics* diag) {
    TrackedBody b = in;
    const double dt = started_ ? time - lastTime_ : 0.0;
    if (!started_ || dt <= 0.0 || dt > 1.0 || in.id != bodyId_) {
        // A new body or a long gap: drop short-term state, keep what was
        // learned about the camera (forward axis, floor)
        seen_.fill(false);
        offsetValid_.fill(false);
        engineState_.reset();
        recentVote_ = recentUsed_ = 0.0;
        disagreeSince_ = agreeSince_ = -1;
        frameSign_ = 0;
        status_.swapped = false;
        pelvisTrack_.clear();
    }
    started_ = true;
    lastTime_ = time;
    bodyId_ = in.id;
    status_.jointsHeld = 0;

    if (options_.fillGaps) holdGaps(b, time);

    // 2D (RGB) data has z == 0 everywhere
    bool any = false, flat = true;
    for (const auto& j : b.joints) {
        if (!usable(j, options_.minConfidence)) continue;
        any = true;
        if (std::fabs(j.z) > 1e-5f) { flat = false; break; }
    }
    if (any) (flat ? planarFrames_ : frames3D_)++;
    status_.planar = planarFrames_ > frames3D_;

    updateForward(b, time, std::max(0.0, dt));
    updateFloor(b, time);
    updateLocomotion(b, time);

    Settings s = settings;
    s.forwardSign = status_.forwardSign;
    if (status_.floor.valid) s.floorUp = status_.floor.up;
    s.plantStanceFoot = status_.overground;
    lastInput_ = b;
    return apply(b, s, diag, &engineState_, time);
}

} // namespace anat
