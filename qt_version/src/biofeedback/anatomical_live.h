#pragma once
// EXPERIMENTAL — frame-by-frame (causal) front end for the anatomical engine,
// for the live patient screen.
//
// The Biofeedback Development tab sees a whole recording, so it can fill
// tracking gaps by interpolating to the next frame, fit the floor and the
// forward axis from every frame, and spot left/right swaps in hindsight.
// Live, only the past is known. LiveProcessor does the same jobs causally:
//   - gap holding: a dropped joint keeps its last offset from its parent
//     joint for up to maxGapSeconds (instead of interpolating)
//   - forward axis: learned from foot-direction votes as frames arrive (or
//     given by the camera type); for 2D data a ~0.5 s recent vote with 0.3 s
//     hysteresis follows the subject turning round
//   - left/right swaps (3D): a recent vote that disagrees with the forward
//     axis for 0.3 s means the tracker swapped sides (subject facing away),
//     and the labels are swapped back until it agrees again for 0.3 s
//   - floor: refitted from recent lowest-foot points (every 1 s for the first
//     ~30 s, then every 5 s)
//   - locomotion: overground (planting on) once the pelvis travels > 1.2 m in
//     a 4 s window, treadmill/standing once it stays within 0.4 m
//   - the engine runs with a TemporalState (vertical rate limits, planting)
//
// One LiveProcessor follows one body; call reset() for a new session.

#include "anatomical_engine.h"

#include <array>
#include <deque>
#include <vector>

namespace anat {

class LiveProcessor {
public:
    enum class Locomotion { Auto = 0, Treadmill = 1, Overground = 2 };

    struct Options {
        bool fillGaps = true;
        double maxGapSeconds = 0.25;
        bool fixLeftRightSwaps = true;
        bool learnFloor = true;        // fit a tilted floor once the subject walked far enough
        int forwardHint = 0;           // sign assumed before the feet have voted (0 = +1)
        Locomotion locomotion = Locomotion::Auto;
        float minConfidence = 0.1f;
    };

    struct Status {
        int forwardSign = 1;      // in use this frame (per-frame for 2D)
        bool forwardLearned = false;
        bool planar = false;
        bool swapped = false;     // left/right labels are being swapped back this frame
        bool overground = false;
        FloorPlane floor;
        int jointsHeld = 0;       // joints filled by gap holding this frame
    };

    void reset();
    void setOptions(const Options& options) { options_ = options; }
    const Options& options() const { return options_; }

    // Process one frame of the followed body. `settings` supplies the gains,
    // neutral, ground contact etc.; forwardSign, floorUp and plantStanceFoot
    // are taken over by the processor.
    TrackedBody process(const TrackedBody& in, double time, const Settings& settings,
                        BodyDiagnostics* diag = nullptr);

    const Status& status() const { return status_; }
    // The body the engine saw this frame (after gap holding and swap correction)
    const TrackedBody& lastInput() const { return lastInput_; }

private:
    void holdGaps(TrackedBody& b, double time);
    void updateForward(TrackedBody& b, double time, double dt);
    void updateFloor(const TrackedBody& b, double time);
    void updateLocomotion(const TrackedBody& b, double time);

    Options options_;
    Status status_;
    TemporalState engineState_;
    TrackedBody lastInput_;

    bool started_ = false;
    double lastTime_ = 0.0;
    uint32_t bodyId_ = 0;

    // gap holding
    std::array<bf::Vec3, JOINT_COUNT> lastPos_{};
    std::array<bf::Vec3, JOINT_COUNT> lastOffset_{};  // from the parent joint
    std::array<double, JOINT_COUNT> lastSeen_{};
    std::array<bool, JOINT_COUNT> seen_{};
    std::array<bool, JOINT_COUNT> offsetValid_{};

    // forward axis
    double totalVote_ = 0.0;
    int totalUsed_ = 0;
    double recentVote_ = 0.0, recentUsed_ = 0.0;  // exponentially decayed
    int planarFrames_ = 0, frames3D_ = 0;
    int frameSign_ = 0;          // 2D: current per-frame sign
    double disagreeSince_ = -1;  // time the recent vote started disagreeing
    double agreeSince_ = -1;

    // floor
    std::deque<bf::Vec3> lowPoints_;
    double lastFloorFit_ = -1e9;

    // locomotion
    std::deque<std::pair<double, bf::Vec3>> pelvisTrack_;
};

} // namespace anat
