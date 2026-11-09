#!/usr/bin/env python3
"""Generate a synthetic walking session in the app's joint-recording CSV format.

For developing biofeedback transforms without patient data: the output loads
in the Playback and Biofeedback Development tabs like a real recording.

    python tools/make_synthetic_gait.py out_dir/synthetic_20260101_120000.csv
    python tools/make_synthetic_gait.py out.csv --camera zed --seconds 20 --overground

The skeleton walks with textbook-shaped sagittal hip/knee/ankle curves plus a
little hip ab/adduction, pelvic sway and tracking jitter. Coordinates follow
each camera's convention (metres):
  kinect — Azure Kinect after the app's Y flip: x right, y up, z away from the
           camera (left-handed). Subject faces the camera.
  zed    — ZED RIGHT_HANDED_Y_UP: x right, y up, z toward the camera.
  rgb    — 2D image-plane data as the RGB models produce (z = 0), side view.
A companion _flags.csv is written next to the session.
"""

import argparse
import math
import os
import random

JOINT_NAMES = [
    "PELVIS", "SPINE_NAVEL", "SPINE_CHEST", "NECK",
    "CLAVICLE_LEFT", "SHOULDER_LEFT", "ELBOW_LEFT", "WRIST_LEFT",
    "HAND_LEFT", "HANDTIP_LEFT", "THUMB_LEFT",
    "CLAVICLE_RIGHT", "SHOULDER_RIGHT", "ELBOW_RIGHT", "WRIST_RIGHT",
    "HAND_RIGHT", "HANDTIP_RIGHT", "THUMB_RIGHT",
    "HIP_LEFT", "KNEE_LEFT", "ANKLE_LEFT", "FOOT_LEFT",
    "HIP_RIGHT", "KNEE_RIGHT", "ANKLE_RIGHT", "FOOT_RIGHT",
    "HEAD", "NOSE", "EYE_LEFT", "EAR_LEFT", "EYE_RIGHT", "EAR_RIGHT",
]
J = {n: i for i, n in enumerate(JOINT_NAMES)}

THIGH, SHANK, FOOT = 0.43, 0.41, 0.15
ANKLE_HEIGHT = 0.075  # ankle joint centre above the floor with the foot flat


def gauss(phase, centre, width):
    """Periodic gaussian bump on a 0..1 gait phase."""
    d = (phase - centre + 0.5) % 1.0 - 0.5
    return math.exp(-0.5 * (d / width) ** 2)


def hip_flexion(p):   # degrees: ~+30 at initial contact, ~-10 at terminal stance
    return 10.0 + 20.0 * math.cos(2 * math.pi * (p - 0.02)) + 4.0 * gauss(p, 0.85, 0.06)


def knee_flexion(p):  # loading-response bump, swing peak ~60
    return 4.0 + 14.0 * gauss(p, 0.14, 0.06) + 56.0 * gauss(p, 0.72, 0.09)


def ankle_dorsiflexion(p):  # + dorsiflexion
    return -6.0 * gauss(p, 0.07, 0.035) + 10.0 * gauss(p, 0.44, 0.10) - 17.0 * gauss(p, 0.62, 0.05) + 3.0 * gauss(p, 0.85, 0.08)


def hip_adduction(p):  # degrees, + adduction in stance
    return 5.0 * math.sin(2 * math.pi * (p + 0.05))


def add(a, b): return (a[0] + b[0], a[1] + b[1], a[2] + b[2])
def mul(a, s): return (a[0] * s, a[1] * s, a[2] * s)


def frame_joints(t, args):
    """Return {joint_index: (ml, up, ant)} in an anatomical frame (metres)."""
    period = args.cycle
    phase_l = (t / period) % 1.0
    phase_r = (phase_l + 0.5) % 1.0
    out = {}

    # Pelvis sway: vertical 2x per cycle, lateral once per cycle
    sway_ml = 0.02 * math.sin(2 * math.pi * phase_l)
    pelvis = (sway_ml, 0.0, 0.0)
    out[J["PELVIS"]] = pelvis
    out[J["SPINE_NAVEL"]] = add(pelvis, (0.0, 0.20, 0.01))
    out[J["SPINE_CHEST"]] = add(pelvis, (0.0, 0.42, 0.02))
    out[J["NECK"]] = add(pelvis, (0.0, 0.62, 0.02))
    out[J["HEAD"]] = add(pelvis, (0.0, 0.76, 0.04))
    out[J["NOSE"]] = add(pelvis, (0.0, 0.78, 0.13))
    out[J["EYE_LEFT"]] = add(pelvis, (-0.035, 0.81, 0.11))
    out[J["EYE_RIGHT"]] = add(pelvis, (0.035, 0.81, 0.11))
    out[J["EAR_LEFT"]] = add(pelvis, (-0.075, 0.79, 0.04))
    out[J["EAR_RIGHT"]] = add(pelvis, (0.075, 0.79, 0.04))

    # Arms swing opposite to the legs
    for side, sgn, phase in (("LEFT", -1.0, phase_r), ("RIGHT", 1.0, phase_l)):
        clav = add(pelvis, (sgn * 0.04, 0.58, 0.02))
        sh = add(pelvis, (sgn * 0.19, 0.56, 0.0))
        swing = math.radians(18.0 * math.cos(2 * math.pi * phase))
        upper = (sgn * 0.02, -math.cos(swing), math.sin(swing))
        el = add(sh, mul(upper, 0.29))
        fore_ang = swing + math.radians(25.0)
        fore = (sgn * 0.01, -math.cos(fore_ang), math.sin(fore_ang))
        wr = add(el, mul(fore, 0.26))
        out[J["CLAVICLE_" + side]] = clav
        out[J["SHOULDER_" + side]] = sh
        out[J["ELBOW_" + side]] = el
        out[J["WRIST_" + side]] = wr
        out[J["HAND_" + side]] = add(wr, mul(fore, 0.07))
        out[J["HANDTIP_" + side]] = add(wr, mul(fore, 0.14))
        out[J["THUMB_" + side]] = add(wr, (sgn * -0.02, -0.04, 0.05))

    # Legs: sagittal angles + frontal-plane ab/adduction
    for side, sgn, phase in (("LEFT", -1.0, phase_l), ("RIGHT", 1.0, phase_r)):
        th = math.radians(hip_flexion(phase) * args.amplitude)
        tk = math.radians(knee_flexion(phase) * args.amplitude)
        ta = math.radians(ankle_dorsiflexion(phase) * args.amplitude)
        add_ = math.radians(hip_adduction(phase) if args.adduction else 0.0)
        hip = add(pelvis, (sgn * 0.095, -0.06, 0.0))
        d_sag = (0.0, -math.cos(th), math.sin(th))
        # adduction tilts the thigh toward the midline (-sgn * ml)
        d = (-sgn * math.sin(add_), d_sag[1] * math.cos(add_), d_sag[2] * math.cos(add_))
        a_t = (0.0, math.sin(th), math.cos(th))
        knee = add(hip, mul(d, THIGH))
        s = add(mul(d, math.cos(tk)), mul(a_t, -math.sin(tk)))
        a_s = add(mul(a_t, math.cos(tk)), mul(d, math.sin(tk)))
        ankle = add(knee, mul(s, SHANK))
        f = add(mul(a_s, math.cos(ta)), mul(s, -math.sin(ta)))
        foot = add(ankle, mul(f, FOOT))
        out[J["HIP_" + side]] = hip
        out[J["KNEE_" + side]] = knee
        out[J["ANKLE_" + side]] = ankle
        out[J["FOOT_" + side]] = foot

    # Put the lowest foot point on the floor (y = 0 in this frame)
    low = min(min(out[J["ANKLE_LEFT"]][1] - ANKLE_HEIGHT, out[J["FOOT_LEFT"]][1] - 0.02),
              min(out[J["ANKLE_RIGHT"]][1] - ANKLE_HEIGHT, out[J["FOOT_RIGHT"]][1] - 0.02))
    travel = args.speed * t if args.overground else 0.0
    return {j: (p[0], p[1] - low, p[2] + travel) for j, p in out.items()}


def pitch_camera(x, y, z, tilt_deg, toward_positive_z):
    """Coordinates seen by a camera pitched down by tilt_deg (rotation about x)."""
    if not tilt_deg:
        return (x, y, z)
    t = math.radians(tilt_deg) * (1.0 if toward_positive_z else -1.0)
    return (x, y * math.cos(t) + z * math.sin(t), -y * math.sin(t) + z * math.cos(t))


def to_camera(p, camera, cam_height, distance, start_ant, tilt=0.0):
    """Anatomical (ml=subject right, up, ant) → camera coordinates."""
    ml, up, ant = p
    y = up - cam_height  # floor is cam_height below the camera
    if camera == "kinect":  # facing camera: subject right = -x, anterior = -z
        return pitch_camera(-ml, y, distance + start_ant - ant, tilt, True)
    if camera == "zed":     # facing camera: subject right = -x, anterior = +z
        return pitch_camera(-ml, y, -(distance + start_ant) + ant, tilt, False)
    # rgb side view in a normalised image plane, walking toward +x, z = 0
    return (ant * 0.8 - 0.2, y * 0.8 + 0.1, 0.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", help="output CSV path (e.g. recordings/synthetic_20260101_120000.csv)")
    ap.add_argument("--camera", choices=["kinect", "zed", "rgb"], default="kinect")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--cycle", type=float, default=1.1, help="gait cycle duration (s)")
    ap.add_argument("--amplitude", type=float, default=1.0, help="scale joint excursions (e.g. 0.6 = shuffling)")
    ap.add_argument("--overground", action="store_true", help="walk toward the camera instead of a treadmill")
    ap.add_argument("--speed", type=float, default=0.6, help="overground speed (m/s)")
    ap.add_argument("--no-adduction", dest="adduction", action="store_false", help="purely sagittal legs")
    ap.add_argument("--noise", type=float, default=0.004, help="tracking jitter SD (m)")
    ap.add_argument("--stand", type=float, default=2.0, help="seconds of quiet standing at the start")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--tilt", type=float, default=0.0, help="camera pitched down by this many degrees (3D only)")
    ap.add_argument("--turn-every", type=float, default=0.0,
                    help="rgb: reverse walking direction every N seconds (subject walks back and forth)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    n = int(args.seconds * args.fps)
    out_dir = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(out_dir, exist_ok=True)
    start_ant = -args.speed * args.seconds if args.overground else 0.0

    with open(args.output, "w", newline="\n") as f:
        f.write("time_seconds,body_id,joint_index,joint_name,x,y,z,confidence\n")
        for i in range(n):
            t = i / args.fps
            # quiet standing first (straight legs), then ease into walking over 1 s
            walk_t = max(0.0, t - args.stand)
            frame_args = argparse.Namespace(**vars(args))
            frame_args.amplitude = args.amplitude * min(1.0, walk_t / 1.0)
            joints = frame_joints(walk_t, frame_args)
            for j in range(len(JOINT_NAMES)):
                jp = joints[j]
                if args.camera == "rgb" and args.turn_every > 0 and int(walk_t / args.turn_every) % 2 == 1:
                    jp = (-jp[0], jp[1], -jp[2])  # facing the other way in the image
                p = to_camera(jp, args.camera, 0.85, 2.6, start_ant, args.tilt)
                if args.camera == "rgb":
                    p = (p[0] + rng.gauss(0, args.noise), p[1] + rng.gauss(0, args.noise), 0.0)
                else:
                    p = tuple(c + rng.gauss(0, args.noise) for c in p)
                conf = 1.0
                if j in (J["FOOT_LEFT"], J["FOOT_RIGHT"]) and rng.random() < 0.03:
                    conf = 0.0  # feet drop out occasionally, as on a real Kinect
                f.write("%.6f,1,%d,%s,%.6f,%.6f,%.6f,%.3f\n" % (t, j, JOINT_NAMES[j], p[0], p[1], p[2], conf))

    stem = args.output[:-4] if args.output.endswith(".csv") else args.output
    with open(stem + "_flags.csv", "w", newline="\n") as f:
        f.write("time_seconds,label\n")
        f.write("%.6f,%s\n" % (0.5, "Quiet stand"))
        f.write("%.6f,%s\n" % (args.stand, "Walk"))
        if args.seconds > args.stand + 10:
            f.write("%.6f,%s\n" % (args.stand + 10, "Mid walk"))
    print("wrote %d frames (%.1f s, %s) to %s" % (n, args.seconds, args.camera, args.output))


if __name__ == "__main__":
    main()
