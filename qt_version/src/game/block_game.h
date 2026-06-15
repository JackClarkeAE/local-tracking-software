#pragma once
#include "../camera/camera_types.h"
#include <vector>
#include <random>
#include <algorithm>
#include <cmath>

enum class GameState { Idle, Playing, GameOver };

struct GameBlock {
    float x, y, z;          // position in game space
    float width, height;    // block dimensions in game units
    float speed;            // units per second along +Z (toward camera)
    float r, g, b;          // block color
    bool alive = true;
};

struct SaberState {
    float posX = 0.0f, posY = 0.0f;    // game-space position of the hand
    float dirX = 0.0f, dirY = 1.0f;    // normalized saber direction
    float tipX = 0.0f, tipY = 0.0f;    // saber tip position
    bool tracked = false;
};

class BlockGame {
public:
    BlockGame() = default;

    void start();
    void stop();
    void reset();

    // Called every frame with deltaTime in seconds and current tracking data
    void update(float dt, const FrameData& frame);

    // Read-only accessors for renderer
    GameState state() const { return state_; }
    int score() const { return score_; }
    int lives() const { return lives_; }
    float elapsedTime() const { return elapsed_; }
    const std::vector<GameBlock>& blocks() const { return blocks_; }
    const SaberState& leftSaber() const { return leftSaber_; }
    const SaberState& rightSaber() const { return rightSaber_; }

private:
    void spawnBlock();
    void updateHandPositions(const FrameData& frame);
    bool checkCollision(const GameBlock& block, const SaberState& saber) const;

    GameState state_ = GameState::Idle;
    int score_ = 0;
    int lives_ = 5;
    float elapsed_ = 0.0f;

    // Spawning
    float spawnTimer_ = 0.0f;
    float spawnInterval_ = 2.0f;

    // Speed ramp
    float baseSpeed_ = 0.3f;
    float lastSpeedBump_ = 0.0f;

    // Game space constants
    static constexpr float Z_FAR = -8.0f;
    static constexpr float Z_NEAR = 0.0f;
    static constexpr float ARENA_HALF_W = 1.75f;
    static constexpr float ARENA_HALF_H = 1.4f;
    static constexpr float BLOCK_W = 0.35f;
    static constexpr float BLOCK_H = 0.35f;
    static constexpr float SABER_LENGTH = 0.4f;
    static constexpr float SABER_RADIUS = 0.18f;
    static constexpr float BODY_SCALE = 2.5f;

    std::vector<GameBlock> blocks_;
    SaberState leftSaber_{};
    SaberState rightSaber_{};
    SaberState lastLeftSaber_{};
    SaberState lastRightSaber_{};

    std::mt19937 rng_{std::random_device{}()};
};
