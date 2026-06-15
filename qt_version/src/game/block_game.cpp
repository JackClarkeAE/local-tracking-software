#include "block_game.h"

// ============================================================================
// Neon color palette for blocks
// ============================================================================
static const float PALETTE[][3] = {
    {1.0f, 0.2f, 0.3f},   // red
    {0.2f, 0.5f, 1.0f},   // blue
    {1.0f, 0.6f, 0.1f},   // orange
    {0.8f, 0.2f, 0.9f},   // purple
    {0.1f, 0.9f, 0.4f},   // green
};
static constexpr int PALETTE_SIZE = 5;

// ============================================================================
// Game state control
// ============================================================================

void BlockGame::start() {
    reset();
    state_ = GameState::Playing;
}

void BlockGame::stop() {
    state_ = GameState::Idle;
}

void BlockGame::reset() {
    state_ = GameState::Idle;
    score_ = 0;
    lives_ = 5;
    elapsed_ = 0.0f;
    spawnTimer_ = 0.0f;
    spawnInterval_ = 2.0f;
    baseSpeed_ = 0.3f;
    lastSpeedBump_ = 0.0f;
    blocks_.clear();
    leftSaber_ = {};
    rightSaber_ = {};
    lastLeftSaber_ = {};
    lastRightSaber_ = {};
}

// ============================================================================
// Main update loop
// ============================================================================

void BlockGame::update(float dt, const FrameData& frame) {
    if (state_ != GameState::Playing) return;

    elapsed_ += dt;

    // ----- Speed ramp: every 10 seconds -----
    if (elapsed_ - lastSpeedBump_ >= 10.0f) {
        baseSpeed_ += 0.02f;
        lastSpeedBump_ = elapsed_;
        spawnInterval_ = std::max(0.5f, spawnInterval_ - 0.1f);
    }

    // ----- Update hand positions from tracking -----
    updateHandPositions(frame);

    // ----- Spawn new blocks -----
    spawnTimer_ += dt;
    if (spawnTimer_ >= spawnInterval_) {
        spawnBlock();
        spawnTimer_ = 0.0f;
    }

    // ----- Move blocks toward camera -----
    for (auto& block : blocks_) {
        if (!block.alive) continue;
        block.z += block.speed * dt;  // moves toward Z_NEAR (0)
    }

    // ----- Check collisions (blocks near saber plane) -----
    for (auto& block : blocks_) {
        if (!block.alive) continue;

        // Block passed the saber plane — successfully dodged
        if (block.z >= Z_NEAR) {
            block.alive = false;
            score_ += 10;
            continue;
        }

        // Only check collision when block is close to saber plane
        if (block.z < -0.6f) continue;

        if (checkCollision(block, leftSaber_) || checkCollision(block, rightSaber_)) {
            block.alive = false;
            lives_ -= 1;
            if (lives_ <= 0) {
                lives_ = 0;
                state_ = GameState::GameOver;
                return;
            }
        }
    }

    // ----- Garbage collect dead blocks -----
    blocks_.erase(
        std::remove_if(blocks_.begin(), blocks_.end(),
                       [](const GameBlock& b) { return !b.alive; }),
        blocks_.end());
}

// ============================================================================
// Block spawning
// ============================================================================

void BlockGame::spawnBlock() {
    GameBlock b{};

    std::uniform_real_distribution<float> xDist(-ARENA_HALF_W + BLOCK_W, ARENA_HALF_W - BLOCK_W);
    std::uniform_real_distribution<float> yDist(-ARENA_HALF_H + BLOCK_H, ARENA_HALF_H - BLOCK_H);
    std::uniform_int_distribution<int> colorDist(0, PALETTE_SIZE - 1);

    b.x = xDist(rng_);
    b.y = yDist(rng_);
    b.z = Z_FAR;
    b.width = BLOCK_W;
    b.height = BLOCK_H;
    b.speed = baseSpeed_;
    b.alive = true;

    int ci = colorDist(rng_);
    b.r = PALETTE[ci][0];
    b.g = PALETTE[ci][1];
    b.b = PALETTE[ci][2];

    blocks_.push_back(b);
}

// ============================================================================
// Hand position extraction from tracking data
// ============================================================================

void BlockGame::updateHandPositions(const FrameData& frame) {
    if (frame.bodies.empty()) return;

    const auto& body = frame.bodies[0];
    const auto& chest = body.joints[(int)CanonicalJoint::SPINE_CHEST];

    // Helper: extract saber state from a hand/wrist/tip joint triple
    auto extractSaber = [&](CanonicalJoint handJ, CanonicalJoint wristJ,
                            CanonicalJoint tipJ) -> SaberState {
        const auto& hand  = body.joints[(int)handJ];
        const auto& wrist = body.joints[(int)wristJ];
        const auto& tip   = body.joints[(int)tipJ];

        SaberState s{};
        if (hand.confidence < 0.1f || chest.confidence < 0.1f) {
            s.tracked = false;
            return s;
        }
        s.tracked = true;

        // Body-relative position scaled to game space
        s.posX = (hand.x - chest.x) * BODY_SCALE;
        s.posY = (hand.y - chest.y) * BODY_SCALE;

        // Saber direction from wrist to handtip
        s.dirX = 0.0f;
        s.dirY = 1.0f;  // default: pointing up
        if (wrist.confidence >= 0.1f && tip.confidence >= 0.1f) {
            float dx = tip.x - wrist.x;
            float dy = tip.y - wrist.y;
            float dz = tip.z - wrist.z;
            float len = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (len > 0.001f) {
                s.dirX = dx / len;
                s.dirY = dy / len;
            }
        }

        // Saber tip in game space
        s.tipX = s.posX + s.dirX * SABER_LENGTH;
        s.tipY = s.posY + s.dirY * SABER_LENGTH;

        return s;
    };

    SaberState left  = extractSaber(CanonicalJoint::HAND_LEFT,
                                     CanonicalJoint::WRIST_LEFT,
                                     CanonicalJoint::HANDTIP_LEFT);
    SaberState right = extractSaber(CanonicalJoint::HAND_RIGHT,
                                     CanonicalJoint::WRIST_RIGHT,
                                     CanonicalJoint::HANDTIP_RIGHT);

    // Freeze on tracking loss
    if (left.tracked)  { leftSaber_ = left;   lastLeftSaber_ = left; }
    else               { leftSaber_ = lastLeftSaber_; }
    if (right.tracked) { rightSaber_ = right;  lastRightSaber_ = right; }
    else               { rightSaber_ = lastRightSaber_; }
}

// ============================================================================
// Collision detection: circle(s) vs axis-aligned rectangle
// ============================================================================

bool BlockGame::checkCollision(const GameBlock& block, const SaberState& saber) const {
    // Test both the hand position and the saber tip against the block rect
    float points[2][2] = {
        { saber.posX, saber.posY },
        { saber.tipX, saber.tipY }
    };

    float bLeft   = block.x - block.width  * 0.5f;
    float bRight  = block.x + block.width  * 0.5f;
    float bBottom = block.y - block.height * 0.5f;
    float bTop    = block.y + block.height * 0.5f;

    for (int i = 0; i < 2; ++i) {
        float px = points[i][0];
        float py = points[i][1];

        // Nearest point on rectangle to circle center
        float nearestX = std::max(bLeft,   std::min(px, bRight));
        float nearestY = std::max(bBottom, std::min(py, bTop));

        float dx = px - nearestX;
        float dy = py - nearestY;

        if (dx * dx + dy * dy < SABER_RADIUS * SABER_RADIUS) {
            return true;
        }
    }

    return false;
}
