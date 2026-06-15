#include "game_renderer.h"
#define _USE_MATH_DEFINES
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// Arena constants (must match BlockGame)
static constexpr float ARENA_HALF_W = 1.75f;
static constexpr float ARENA_HALF_H = 1.4f;
static constexpr float Z_FAR = -8.0f;

// ============================================================================
// Lifecycle
// ============================================================================

GameRenderer::~GameRenderer() {
    shutdown();
}

bool GameRenderer::init(int width, int height) {
    if (initialized_) return true;
    width_ = width;
    height_ = height;
    createFBO();
    initialized_ = true;
    return true;
}

void GameRenderer::shutdown() {
    if (!initialized_) return;
    destroyFBO();
    initialized_ = false;
}

// ============================================================================
// FBO management (same pattern as SkeletonRenderer)
// ============================================================================

void GameRenderer::createFBO() {
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // Color texture
    glGenTextures(1, &colorTexture_);
    glBindTexture(GL_TEXTURE_2D, colorTexture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width_, height_, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, colorTexture_, 0);

    // Depth renderbuffer
    glGenRenderbuffers(1, &depthRBO_);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRBO_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depthRBO_);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GameRenderer::destroyFBO() {
    if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (colorTexture_) { glDeleteTextures(1, &colorTexture_); colorTexture_ = 0; }
    if (depthRBO_) { glDeleteRenderbuffers(1, &depthRBO_); depthRBO_ = 0; }
}

// ============================================================================
// Projection setup
// ============================================================================

void GameRenderer::setupProjection() {
    double aspect = (double)width_ / height_;
    double fovY = 50.0;  // degrees
    double tanFov = tan(fovY * M_PI / 360.0);
    double nearP = 0.1, farP = 20.0;
    double top = nearP * tanFov;
    double right = top * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, nearP, farP);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    // Camera at origin looking down -Z. Game coords are in camera space.
}

// ============================================================================
// Main render
// ============================================================================

void GameRenderer::render(const BlockGame& game) {
    if (!initialized_) return;

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);

    // Dark space background
    glClearColor(0.02f, 0.02f, 0.06f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_POINT_SMOOTH);
    glEnable(GL_DEPTH_TEST);

    setupProjection();

    // Draw corridor grid
    drawCorridor();

    // Draw blocks (depth-tested)
    for (const auto& block : game.blocks()) {
        drawBlock(block);
    }

    // Draw sabers on top (disable depth test so they're always visible)
    glDisable(GL_DEPTH_TEST);
    drawSaber(game.leftSaber(),  0.0f, 0.9f, 1.0f);   // cyan
    drawSaber(game.rightSaber(), 1.0f, 0.15f, 0.15f);  // red

    // Cleanup GL state
    glDisable(GL_BLEND);
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_POINT_SMOOTH);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ============================================================================
// Corridor / tunnel grid
// ============================================================================

void GameRenderer::drawCorridor() {
    glLineWidth(1.0f);
    glBegin(GL_LINES);

    // --- Horizontal floor/ceiling lines at various Z depths ---
    for (int i = 0; i <= 16; ++i) {
        float z = -0.5f * i;  // Z = 0 to -8
        float alpha = 1.0f - (float)i / 16.0f;
        alpha *= 0.3f;

        glColor4f(0.1f, 0.3f, 0.5f, alpha);

        // Floor
        glVertex3f(-ARENA_HALF_W, -ARENA_HALF_H, z);
        glVertex3f( ARENA_HALF_W, -ARENA_HALF_H, z);

        // Ceiling
        glVertex3f(-ARENA_HALF_W,  ARENA_HALF_H, z);
        glVertex3f( ARENA_HALF_W,  ARENA_HALF_H, z);
    }

    // --- Vertical rails along floor and ceiling ---
    int numRails = 8;
    for (int i = 0; i <= numRails; ++i) {
        float t = (float)i / numRails;
        float x = -ARENA_HALF_W + t * (2.0f * ARENA_HALF_W);

        // Floor rail (near to far)
        glColor4f(0.1f, 0.3f, 0.5f, 0.3f);
        glVertex3f(x, -ARENA_HALF_H, 0.0f);
        glColor4f(0.1f, 0.3f, 0.5f, 0.02f);
        glVertex3f(x, -ARENA_HALF_H, Z_FAR);

        // Ceiling rail
        glColor4f(0.1f, 0.3f, 0.5f, 0.3f);
        glVertex3f(x,  ARENA_HALF_H, 0.0f);
        glColor4f(0.1f, 0.3f, 0.5f, 0.02f);
        glVertex3f(x,  ARENA_HALF_H, Z_FAR);
    }

    // --- Side wall lines (left and right edges) ---
    int numSideLines = 8;
    for (int i = 0; i <= numSideLines; ++i) {
        float t = (float)i / numSideLines;
        float y = -ARENA_HALF_H + t * (2.0f * ARENA_HALF_H);

        // Left wall
        glColor4f(0.1f, 0.3f, 0.5f, 0.2f);
        glVertex3f(-ARENA_HALF_W, y, 0.0f);
        glColor4f(0.1f, 0.3f, 0.5f, 0.02f);
        glVertex3f(-ARENA_HALF_W, y, Z_FAR);

        // Right wall
        glColor4f(0.1f, 0.3f, 0.5f, 0.2f);
        glVertex3f( ARENA_HALF_W, y, 0.0f);
        glColor4f(0.1f, 0.3f, 0.5f, 0.02f);
        glVertex3f( ARENA_HALF_W, y, Z_FAR);
    }

    glEnd();
}

// ============================================================================
// Block rendering
// ============================================================================

void GameRenderer::drawBlock(const GameBlock& block) {
    float hw = block.width * 0.5f;
    float hh = block.height * 0.5f;
    float depth = 0.15f;

    // --- Front face (filled quad) ---
    glBegin(GL_QUADS);
    glColor4f(block.r, block.g, block.b, 0.85f);
    glVertex3f(block.x - hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y + hh, block.z);
    glVertex3f(block.x - hw, block.y + hh, block.z);
    glEnd();

    // --- Bright outline ---
    glLineWidth(2.0f);
    glBegin(GL_LINE_LOOP);
    float br = std::min(block.r * 1.3f, 1.0f);
    float bg = std::min(block.g * 1.3f, 1.0f);
    float bb = std::min(block.b * 1.3f, 1.0f);
    glColor4f(br, bg, bb, 1.0f);
    glVertex3f(block.x - hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y + hh, block.z);
    glVertex3f(block.x - hw, block.y + hh, block.z);
    glEnd();

    // --- Side edges for 3D depth illusion ---
    glBegin(GL_QUADS);
    glColor4f(block.r * 0.5f, block.g * 0.5f, block.b * 0.5f, 0.7f);

    // Right edge
    glVertex3f(block.x + hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y - hh, block.z - depth);
    glVertex3f(block.x + hw, block.y + hh, block.z - depth);
    glVertex3f(block.x + hw, block.y + hh, block.z);

    // Bottom edge
    glVertex3f(block.x - hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y - hh, block.z);
    glVertex3f(block.x + hw, block.y - hh, block.z - depth);
    glVertex3f(block.x - hw, block.y - hh, block.z - depth);
    glEnd();
}

// ============================================================================
// Saber rendering
// ============================================================================

void GameRenderer::drawSaber(const SaberState& saber, float r, float g, float b) {
    // Don't draw if never tracked
    if (!saber.tracked && saber.posX == 0.0f && saber.posY == 0.0f) return;

    float saberZ = -0.1f;  // slightly in front of camera for visibility

    // --- Thick beam (outer glow) ---
    glLineWidth(6.0f);
    glBegin(GL_LINES);
    glColor4f(r, g, b, 1.0f);
    glVertex3f(saber.posX, saber.posY, saberZ);
    glVertex3f(saber.tipX, saber.tipY, saberZ);
    glEnd();

    // --- Bright core (inner white-ish line) ---
    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor4f(r * 0.5f + 0.5f, g * 0.5f + 0.5f, b * 0.5f + 0.5f, 0.9f);
    glVertex3f(saber.posX, saber.posY, saberZ);
    glVertex3f(saber.tipX, saber.tipY, saberZ);
    glEnd();

    // --- Glow circles ---
    drawGlowCircle(saber.tipX, saber.tipY, saberZ, 0.08f, r, g, b, 0.5f);
    drawGlowCircle(saber.posX, saber.posY, saberZ, 0.06f, r, g, b, 0.3f);
}

// ============================================================================
// Glow circle (radial gradient, transparent at edges)
// ============================================================================

void GameRenderer::drawGlowCircle(float cx, float cy, float z, float radius,
                                   float r, float g, float b, float alpha) {
    int segments = 24;
    glBegin(GL_TRIANGLE_FAN);

    // Center (bright)
    glColor4f(r, g, b, alpha);
    glVertex3f(cx, cy, z);

    // Edges (transparent)
    glColor4f(r, g, b, 0.0f);
    for (int i = 0; i <= segments; ++i) {
        float angle = 2.0f * (float)M_PI * (float)i / (float)segments;
        glVertex3f(cx + radius * cosf(angle), cy + radius * sinf(angle), z);
    }

    glEnd();
}
