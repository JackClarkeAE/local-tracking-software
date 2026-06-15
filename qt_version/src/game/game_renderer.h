#pragma once
#include "block_game.h"
#include <GL/glew.h>

class GameRenderer {
public:
    GameRenderer() = default;
    ~GameRenderer();

    bool init(int width, int height);
    void shutdown();

    // Render the full game scene to FBO
    void render(const BlockGame& game);

    GLuint getTexture() const { return colorTexture_; }
    int getWidth() const { return width_; }
    int getHeight() const { return height_; }

private:
    void createFBO();
    void destroyFBO();
    void setupProjection();
    void drawCorridor();
    void drawBlock(const GameBlock& block);
    void drawSaber(const SaberState& saber, float r, float g, float b);
    void drawGlowCircle(float cx, float cy, float z, float radius,
                        float r, float g, float b, float alpha);

    GLuint fbo_ = 0;
    GLuint colorTexture_ = 0;
    GLuint depthRBO_ = 0;
    int width_ = 0, height_ = 0;
    bool initialized_ = false;
};
