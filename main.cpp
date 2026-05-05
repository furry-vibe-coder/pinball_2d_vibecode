#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#if __has_include(<SDL2/SDL_mixer.h>)
#include <SDL2/SDL_mixer.h>
#define PINBALL_USE_MIXER 1
#else
#define PINBALL_USE_MIXER 0
struct Mix_Chunk {};
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace fs = std::filesystem;

static constexpr int kScreenW = 960;
static constexpr int kScreenH = 1280;
static constexpr float kFixedDt = 1.0f / 120.0f;
static constexpr float kPi = 3.14159265358979323846f;
static constexpr int kMaxMultiballs = 4096;
static constexpr int kMaxSpawnPerWave = 128;
static constexpr const char *kHighScoreFile = "pinball_highscore.txt";

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

static Vec2 operator+(const Vec2 &a, const Vec2 &b) { return {a.x + b.x, a.y + b.y}; }
static Vec2 operator-(const Vec2 &a, const Vec2 &b) { return {a.x - b.x, a.y - b.y}; }
static Vec2 operator*(const Vec2 &a, float s) { return {a.x * s, a.y * s}; }
static Vec2 operator/(const Vec2 &a, float s) { return {a.x / s, a.y / s}; }
static Vec2 &operator+=(Vec2 &a, const Vec2 &b) {
    a.x += b.x;
    a.y += b.y;
    return a;
}
static Vec2 &operator-=(Vec2 &a, const Vec2 &b) {
    a.x -= b.x;
    a.y -= b.y;
    return a;
}
static float Dot(const Vec2 &a, const Vec2 &b) { return a.x * b.x + a.y * b.y; }
static float Length(const Vec2 &v) { return std::sqrt(Dot(v, v)); }
static Vec2 Normalize(const Vec2 &v) {
    float len = Length(v);
    if (len <= 1e-6f) {
        return {0.0f, -1.0f};
    }
    return v / len;
}
static float Lerp(float a, float b, float t) { return a + (b - a) * t; }
static float SmoothStep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
static Vec2 Perp(const Vec2 &v) { return {-v.y, v.x}; }
static float Fract(float v) { return v - std::floor(v); }
static float Hash21(float x, float y) {
    float n = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return Fract(n);
}
static float Noise2(float x, float y) {
    float ix = std::floor(x);
    float iy = std::floor(y);
    float fx = x - ix;
    float fy = y - iy;
    float u = fx * fx * (3.0f - 2.0f * fx);
    float v = fy * fy * (3.0f - 2.0f * fy);
    float a = Hash21(ix, iy);
    float b = Hash21(ix + 1.0f, iy);
    float c = Hash21(ix, iy + 1.0f);
    float d = Hash21(ix + 1.0f, iy + 1.0f);
    return Lerp(Lerp(a, b, u), Lerp(c, d, u), v);
}
static float Fbm2(float x, float y) {
    float sum = 0.0f;
    float amp = 0.55f;
    float freq = 1.0f;
    for (int i = 0; i < 5; ++i) {
        sum += Noise2(x * freq, y * freq) * amp;
        freq *= 2.07f;
        amp *= 0.52f;
    }
    return sum;
}

struct Segment {
    Vec2 a;
    Vec2 b;
    float bounce = 0.75f;
};

struct Ball {
    Vec2 pos{880.0f, 1100.0f};
    Vec2 vel{0.0f, 0.0f};
    float radius = 14.0f;
    float squash = 0.0f;
    float ttl = -1.0f;
    float maxTtl = -1.0f;
};

struct Bumper {
    Vec2 pos;
    float radius = 42.0f;
    float flash = 0.0f;
    float springK = 2100.0f;
    int score = 10;
};

struct FloatingText {
    Vec2 pos;
    Vec2 vel;
    std::string text;
    float ttl = 0.5f;
    float maxTtl = 0.5f;
};

struct SaucerKicker {
    Vec2 pos{680.0f, 780.0f};
    float radius = 28.0f;
    bool captured = false;
    bool capturedIsMain = true;
    Ball capturedBall{};
    float holdTimer = 0.0f;
    float recaptureCooldown = 0.0f;
};

struct Flipper {
    Vec2 pivot;
    float length = 130.0f;
    float width = 26.0f;
    float restAngle = 0.0f;
    float activeAngle = 0.0f;
    float currentAngle = 0.0f;
    bool isLeft = true;
    bool pressed = false;
    float angularVel = 0.0f;
};

struct Plunger {
    float yTop = 940.0f;
    float yBottom = 1180.0f;
    float yCurrent = 1180.0f;
    float maxCompression = 170.0f;
    float pullAmount = 0.0f;
    float pullSpeed = 1.8f;
    float returnSpeed = 7.0f;
    bool held = false;
};

struct ToneVoice {
    bool active = false;
    float freq = 440.0f;
    float gain = 0.2f;
    float phase = 0.0f;
    float t = 0.0f;
    float duration = 0.1f;
    float attack = 0.004f;
    float release = 0.05f;
    float harm2 = 0.3f;
    float harm3 = 0.15f;
};

struct Synth {
    SDL_AudioDeviceID device = 0;
    SDL_AudioSpec obtained{};
    std::array<ToneVoice, 32> voices{};

    static void AudioCallback(void *userdata, Uint8 *stream, int len) {
        auto *self = static_cast<Synth *>(userdata);
        int samples = len / static_cast<int>(sizeof(float));
        float *out = reinterpret_cast<float *>(stream);
        for (int i = 0; i < samples; ++i) {
            float mix = 0.0f;
            for (auto &v : self->voices) {
                if (!v.active) {
                    continue;
                }
                v.t += 1.0f / static_cast<float>(self->obtained.freq);
                if (v.t >= v.duration) {
                    v.active = false;
                    continue;
                }
                float env = 1.0f;
                if (v.t < v.attack) {
                    env = v.t / std::max(0.0001f, v.attack);
                } else if (v.t > v.duration - v.release) {
                    env = (v.duration - v.t) / std::max(0.0001f, v.release);
                }
                v.phase += (2.0f * kPi * v.freq) / static_cast<float>(self->obtained.freq);
                if (v.phase > 2.0f * kPi) {
                    v.phase -= 2.0f * kPi;
                }
                float s = std::sin(v.phase);
                s += std::sin(v.phase * 2.0f) * v.harm2;
                s += std::sin(v.phase * 3.0f) * v.harm3;
                mix += s * env * v.gain;
            }
            out[i] = std::clamp(mix, -0.9f, 0.9f);
        }
    }

    bool Init() {
        SDL_AudioSpec desired{};
        desired.freq = 48000;
        desired.format = AUDIO_F32SYS;
        desired.channels = 1;
        desired.samples = 1024;
        desired.callback = AudioCallback;
        desired.userdata = this;
        device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
        if (!device) {
            std::cerr << "SDL_OpenAudioDevice warning: " << SDL_GetError() << "\n";
            return false;
        }
        SDL_PauseAudioDevice(device, 0);
        return true;
    }

    void Shutdown() {
        if (device) {
            SDL_CloseAudioDevice(device);
            device = 0;
        }
    }

    void Play(float freq, float duration, float gain, float harm2 = 0.3f, float harm3 = 0.12f) {
        if (!device) {
            return;
        }
        SDL_LockAudioDevice(device);
        for (auto &v : voices) {
            if (!v.active) {
                v.active = true;
                v.freq = freq;
                v.duration = duration;
                v.gain = gain;
                v.phase = 0.0f;
                v.t = 0.0f;
                v.attack = std::min(0.01f, duration * 0.18f);
                v.release = std::max(0.02f, duration * 0.38f);
                v.harm2 = harm2;
                v.harm3 = harm3;
                break;
            }
        }
        SDL_UnlockAudioDevice(device);
    }
};

#ifdef __EMSCRIPTEN__
EM_JS(int, WebLoadHighScore, (), {
  try {
    const raw = localStorage.getItem('pinball_highscore');
    const v = raw === null ? 0 : (parseInt(raw, 10) || 0);
    return v < 0 ? 0 : v;
  } catch (e) {
    return 0;
  }
});

EM_JS(void, WebSaveHighScore, (int value), {
  try {
    localStorage.setItem('pinball_highscore', String(value < 0 ? 0 : value));
  } catch (e) {}
});
#endif

static int LoadHighScore() {
#ifdef __EMSCRIPTEN__
    return WebLoadHighScore();
#else
    std::ifstream f(kHighScoreFile);
    int v = 0;
    if (f >> v) {
        return std::max(0, v);
    }
    return 0;
#endif
}

static void SaveHighScore(int value) {
#ifdef __EMSCRIPTEN__
    WebSaveHighScore(value);
#else
    std::ofstream f(kHighScoreFile, std::ios::trunc);
    if (f) {
        f << std::max(0, value) << "\n";
    }
#endif
}

static void DrawFilledCircle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    for (int y = -radius; y <= radius; ++y) {
        int dx = static_cast<int>(std::sqrt(radius * radius - y * y));
        SDL_RenderDrawLine(renderer, cx - dx, cy + y, cx + dx, cy + y);
    }
}

static void DrawSoftCircle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    for (int i = 4; i >= 1; --i) {
        float k = static_cast<float>(i) / 4.0f;
        int r = static_cast<int>(radius * (0.45f + 0.75f * k));
        Uint8 a = static_cast<Uint8>(color.a * (0.16f + 0.22f * k));
        DrawFilledCircle(renderer, cx, cy, r, {color.r, color.g, color.b, a});
    }
    DrawFilledCircle(renderer, cx, cy, static_cast<int>(radius * 0.55f), {color.r, color.g, color.b, static_cast<Uint8>(color.a * 0.28f)});
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}

using Glyph = std::array<uint8_t, 7>;

static Glyph GetGlyph(char c) {
    switch (c) {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E};
    case 'D': return {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'J': return {0x07, 0x02, 0x02, 0x02, 0x12, 0x12, 0x0C};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '0': return {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E};
    case '1': return {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E};
    case '2': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F};
    case '3': return {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E};
    case '4': return {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02};
    case '5': return {0x1F, 0x10, 0x10, 0x1E, 0x01, 0x01, 0x1E};
    case '6': return {0x0E, 0x10, 0x10, 0x1E, 0x11, 0x11, 0x0E};
    case '7': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08};
    case '8': return {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E};
    case '9': return {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x01, 0x0E};
    case '+': return {0x00, 0x04, 0x04, 0x1F, 0x04, 0x04, 0x00};
    case '-': return {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00};
    case ':': return {0x00, 0x04, 0x00, 0x00, 0x04, 0x00, 0x00};
    case '.': return {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06};
    default: return {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    }
}

static void DrawBitmapText(SDL_Renderer *renderer, const std::string &text, int x, int y, int scale, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    int cursor = x;
    for (char raw : text) {
        char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        if (c == ' ') {
            cursor += 4 * scale;
            continue;
        }
        Glyph g = GetGlyph(c);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((g[row] >> (4 - col)) & 1) {
                    SDL_Rect px{cursor + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &px);
                }
            }
        }
        cursor += 6 * scale;
    }
}

static int BitmapTextWidth(const std::string &text, int scale) {
    int w = 0;
    for (char c : text) {
        if (c == ' ') {
            w += 4 * scale;
        } else {
            w += 6 * scale;
        }
    }
    return w;
}

static void DrawCenteredText(SDL_Renderer *renderer, TTF_Font *font, const std::string &text, int centerX, int y, SDL_Color color,
                             int bitmapScale = 4) {
    if (!font) {
        int w = BitmapTextWidth(text, bitmapScale);
        DrawBitmapText(renderer, text, centerX - w / 2, y, bitmapScale, color);
        return;
    }
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) {
        return;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_Rect dst{centerX - surf->w / 2, y, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
}

static void DrawGlow(SDL_Renderer *renderer, const Vec2 &p, float radius, SDL_Color col) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    for (int i = 4; i >= 1; --i) {
        float r = radius * (0.5f + i * 0.45f);
        Uint8 a = static_cast<Uint8>(30.0f + i * 14.0f);
        DrawFilledCircle(renderer, static_cast<int>(p.x), static_cast<int>(p.y), static_cast<int>(r), {col.r, col.g, col.b, a});
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
}

static void DrawFancyTableFx(SDL_Renderer *renderer, const SDL_Rect &table, float t, float pulse) {
    // Heavy pseudo-Perlin refraction bands with thicker smooth strips.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_ADD);
    for (int y = table.y; y < table.y + table.h; y += 6) {
        float fy = static_cast<float>(y - table.y) / std::max(1, table.h);
        float n1 = Fbm2(fy * 8.0f + t * 0.28f, t * 0.22f);
        float n2 = Fbm2(fy * 12.0f - t * 0.31f, 11.3f + t * 0.19f);
        float turb = (n1 * 2.0f - 1.0f) + (n2 * 2.0f - 1.0f) * 0.7f;
        int xOff = static_cast<int>(turb * (28.0f + 14.0f * pulse));
        float turbulence = std::clamp(std::abs(turb), 0.0f, 1.0f);
        Uint8 aCore = static_cast<Uint8>(28.0f + 40.0f * turbulence);
        Uint8 aEdge = static_cast<Uint8>(10.0f + 16.0f * turbulence);
        SDL_SetRenderDrawColor(renderer, 85, 165, 235, aEdge);
        SDL_RenderDrawLine(renderer, table.x + 8 + xOff, y - 2, table.x + table.w - 8 + xOff, y - 2);
        SDL_RenderDrawLine(renderer, table.x + 8 + xOff, y + 2, table.x + table.w - 8 + xOff, y + 2);
        SDL_SetRenderDrawColor(renderer, 105, 185, 255, aCore);
        SDL_RenderDrawLine(renderer, table.x + 8 + xOff, y, table.x + table.w - 8 + xOff, y);
        SDL_RenderDrawLine(renderer, table.x + 8 + xOff, y + 1, table.x + table.w - 8 + xOff, y + 1);
    }

    // Smoke-like blobs drifting through playfield (soft blurred haze).
    for (int i = 0; i < 24; ++i) {
        float fi = static_cast<float>(i);
        float nx = Fbm2(fi * 1.5f + t * 0.12f, 3.2f + t * 0.08f);
        float ny = Fbm2(fi * 2.1f - t * 0.10f, 8.7f + t * 0.06f);
        float x = table.x + 30.0f + nx * (table.w - 60.0f);
        float y = table.y + 40.0f + ny * (table.h - 80.0f);
        int r = static_cast<int>(34.0f + 58.0f * Noise2(fi * 0.73f, t * 0.13f + fi));
        Uint8 a = static_cast<Uint8>(16.0f + 34.0f * Noise2(fi * 1.1f + t * 0.10f, 4.0f + fi));
        DrawSoftCircle(renderer, static_cast<int>(x), static_cast<int>(y), r, {90, 145, 185, a});
    }

    // Vignette-ish edge darkening for contrast.
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 22; ++i) {
        Uint8 a = static_cast<Uint8>(5 + i);
        SDL_SetRenderDrawColor(renderer, 4, 8, 16, a);
        SDL_Rect r{table.x + i, table.y + i, table.w - 2 * i, table.h - 2 * i};
        SDL_RenderDrawRect(renderer, &r);
    }
}

static void DrawVelocityTail(SDL_Renderer *renderer, const Ball &b, SDL_Color col) {
    float speed = Length(b.vel);
    if (speed < 20.0f) {
        return;
    }
    Vec2 dir = Normalize(b.vel);
    int steps = std::clamp(static_cast<int>(speed / 160.0f), 4, 10);
    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / static_cast<float>(steps);
        Vec2 p = b.pos - dir * (6.0f + 10.0f * t * static_cast<float>(steps));
        float lifeAlpha = 1.0f;
        if (b.maxTtl > 0.0f && b.ttl >= 0.0f) {
            lifeAlpha = std::clamp(b.ttl / b.maxTtl, 0.0f, 1.0f);
        }
        Uint8 a = static_cast<Uint8>((1.0f - t) * col.a * lifeAlpha);
        int r = std::max(2, static_cast<int>(b.radius * (0.22f + 0.18f * (1.0f - t))));
        DrawFilledCircle(renderer, static_cast<int>(p.x), static_cast<int>(p.y), r, {col.r, col.g, col.b, a});
    }
}

static float DistanceToSegment(const Vec2 &p, const Segment &s, Vec2 &closest, float &tOut) {
    Vec2 ab = s.b - s.a;
    float denom = Dot(ab, ab);
    float t = 0.0f;
    if (denom > 1e-6f) {
        t = Dot(p - s.a, ab) / denom;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    closest = s.a + ab * t;
    tOut = t;
    return Length(p - closest);
}

static bool ResolveBallSegment(Ball &ball, const Segment &seg, float *impactOut = nullptr) {
    Vec2 cp;
    float t = 0.0f;
    float dist = DistanceToSegment(ball.pos, seg, cp, t);
    if (dist < ball.radius) {
        Vec2 normal = Normalize(ball.pos - cp);
        float penetration = ball.radius - dist + 0.1f;
        ball.pos += normal * penetration;

        float vn = Dot(ball.vel, normal);
        if (vn < 0.0f) {
            ball.vel -= normal * ((1.0f + seg.bounce) * vn);
            ball.vel = ball.vel * 0.995f;
            ball.squash = std::min(1.0f, ball.squash + std::abs(vn) * 0.02f);
            if (impactOut) {
                *impactOut = std::max(*impactOut, std::abs(vn));
            }
        }
        return true;
    }
    return false;
}

static bool ResolveBallBumper(Ball &ball, Bumper &bumper, int &score) {
    Vec2 d = ball.pos - bumper.pos;
    float dist = Length(d);
    float target = ball.radius + bumper.radius;
    if (dist < target) {
        Vec2 normal = Normalize(d);
        float penetration = target - dist + 0.1f;
        ball.pos += normal * penetration;

        float compression = penetration;
        float springImpulse = bumper.springK * compression * kFixedDt;
        ball.vel += normal * springImpulse;

        float vn = Dot(ball.vel, normal);
        if (vn < 0.0f) {
            ball.vel -= normal * (1.65f * vn);
        }

        bumper.flash = 1.0f;
        score += bumper.score;
        ball.squash = std::min(1.0f, ball.squash + 0.28f);
        return true;
    }
    return false;
}

static void ResolveBallBall(Ball &a, Ball &b) {
    Vec2 d = b.pos - a.pos;
    float dist = Length(d);
    float target = a.radius + b.radius;
    if (dist <= 1e-5f || dist >= target) {
        return;
    }
    Vec2 n = d / dist;
    float penetration = target - dist + 0.05f;
    a.pos -= n * (penetration * 0.5f);
    b.pos += n * (penetration * 0.5f);

    Vec2 rel = b.vel - a.vel;
    float vn = Dot(rel, n);
    if (vn < 0.0f) {
        float restitution = 0.85f;
        float j = -(1.0f + restitution) * vn * 0.5f;
        Vec2 impulse = n * j;
        a.vel -= impulse;
        b.vel += impulse;
        a.squash = std::min(1.0f, a.squash + std::abs(vn) * 0.02f);
        b.squash = std::min(1.0f, b.squash + std::abs(vn) * 0.02f);
    }
}

static Vec2 FlipperTip(const Flipper &f) {
    return {f.pivot.x + std::cos(f.currentAngle) * f.length, f.pivot.y + std::sin(f.currentAngle) * f.length};
}

static void ResolveBallFlipper(Ball &ball, Flipper &flipper) {
    Segment flipperSegment{flipper.pivot, FlipperTip(flipper), 0.8f};
    Vec2 cp;
    float t = 0.0f;
    float dist = DistanceToSegment(ball.pos, flipperSegment, cp, t);
    if (dist < ball.radius + flipper.width * 0.4f) {
        Vec2 normal = Normalize(ball.pos - cp);
        float penetration = ball.radius + flipper.width * 0.4f - dist + 0.1f;
        ball.pos += normal * penetration;

        Vec2 tangent = Normalize(flipperSegment.b - flipperSegment.a);
        Vec2 flipperVelAtPoint = Perp(tangent) * (flipper.angularVel * flipper.length * t);
        Vec2 rel = ball.vel - flipperVelAtPoint;
        float vn = Dot(rel, normal);
        if (vn < 0.0f) {
            // Flippers should "cradle" more than walls: damp normal bounce, preserve controlled tangent flow.
            Vec2 relN = normal * vn;
            Vec2 relT = rel - relN;
            relN = relN * -0.20f;
            relT = relT * 0.83f;
            float drive = flipper.pressed ? 125.0f : 18.0f;
            ball.vel = relN + relT + flipperVelAtPoint + normal * drive;
            ball.squash = std::min(1.0f, ball.squash + std::abs(vn) * 0.04f);
        }
    }
}

static void UpdateFlipper(Flipper &f, float dt) {
    float target = f.pressed ? f.activeAngle : f.restAngle;
    float prev = f.currentAngle;
    float t = SmoothStep01(dt * 28.0f);
    f.currentAngle = Lerp(f.currentAngle, target, t);
    f.angularVel = (f.currentAngle - prev) / dt;
}

static void UpdatePlunger(Plunger &p, float dt) {
    if (p.held) {
        p.pullAmount = std::min(1.0f, p.pullAmount + p.pullSpeed * dt);
    } else {
        p.pullAmount = std::max(0.0f, p.pullAmount - p.returnSpeed * dt);
    }
    p.yCurrent = p.yBottom - p.pullAmount * p.maxCompression;
}

static void DrawFlipper(SDL_Renderer *renderer, const Flipper &f, SDL_Color col) {
    Vec2 tip = FlipperTip(f);
    Vec2 dir = Normalize(tip - f.pivot);
    Vec2 n = Perp(dir) * (f.width * 0.5f);
    SDL_Vertex verts[4];
    verts[0].position = {f.pivot.x + n.x, f.pivot.y + n.y};
    verts[1].position = {f.pivot.x - n.x, f.pivot.y - n.y};
    verts[2].position = {tip.x - n.x, tip.y - n.y};
    verts[3].position = {tip.x + n.x, tip.y + n.y};
    for (auto &v : verts) {
        v.color = col;
        v.tex_coord = {0.0f, 0.0f};
    }
    const int idx[] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(renderer, nullptr, verts, 4, idx, 6);
    DrawFilledCircle(renderer, static_cast<int>(f.pivot.x), static_cast<int>(f.pivot.y), static_cast<int>(f.width * 0.55f), col);
}

static void DrawHud(SDL_Renderer *renderer, TTF_Font *font, int score, int highScore, int ballsLeft, bool gameOver, bool showHelp) {
    SDL_Color color{232, 240, 255, 255};
    std::string text =
        "High " + std::to_string(highScore) + "   Score " + std::to_string(score) + "   Balls " + std::to_string(ballsLeft);
    if (!font) {
        DrawBitmapText(renderer, text, 34, 24, 4, color);
        if (gameOver) {
            DrawCenteredText(renderer, font, "GAME OVER", kScreenW / 2, kScreenH / 2 - 70, {255, 180, 120, 255}, 7);
            DrawCenteredText(renderer, font, "PRESS SPACE OR ENTER TO RESTART", kScreenW / 2, kScreenH / 2 - 16,
                             {255, 205, 150, 255}, 3);
        } else if (showHelp) {
            DrawCenteredText(renderer, font, "DOWN OR SPACE OR ENTER: LAUNCH BALL", kScreenW / 2, kScreenH / 2 - 48,
                             {220, 235, 255, 220}, 3);
            DrawCenteredText(renderer, font, "LEFT/RIGHT: FLIPPERS", kScreenW / 2, kScreenH / 2 - 14, {220, 235, 255, 220}, 3);
        }
        return;
    }
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text.c_str(), color);
    if (!surf) {
        return;
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_Rect dst{34, 24, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (tex) {
        SDL_RenderCopy(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    if (gameOver) {
        DrawCenteredText(renderer, font, "GAME OVER", kScreenW / 2, kScreenH / 2 - 70, {255, 180, 120, 255}, 7);
        DrawCenteredText(renderer, font, "PRESS SPACE OR ENTER TO RESTART", kScreenW / 2, kScreenH / 2 - 16,
                         {255, 205, 150, 255}, 3);
    } else if (showHelp) {
        DrawCenteredText(renderer, font, "DOWN OR SPACE OR ENTER: LAUNCH BALL", kScreenW / 2, kScreenH / 2 - 48,
                         {220, 235, 255, 220}, 3);
        DrawCenteredText(renderer, font, "LEFT/RIGHT: FLIPPERS", kScreenW / 2, kScreenH / 2 - 14, {220, 235, 255, 220}, 3);
    }
}

static void DrawFloatingTexts(SDL_Renderer *renderer, TTF_Font *font, const std::vector<FloatingText> &texts) {
    for (const auto &ft : texts) {
        float alphaT = std::clamp(ft.ttl / std::max(0.001f, ft.maxTtl), 0.0f, 1.0f);
        SDL_Color c{255, 220, 140, static_cast<Uint8>(255.0f * alphaT)};
        if (!font) {
            DrawBitmapText(renderer, ft.text, static_cast<int>(ft.pos.x), static_cast<int>(ft.pos.y), 3, c);
            continue;
        }
        SDL_Surface *s = TTF_RenderUTF8_Blended(font, ft.text.c_str(), c);
        if (!s) {
            continue;
        }
        SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
        SDL_Rect d{static_cast<int>(ft.pos.x - s->w / 2), static_cast<int>(ft.pos.y - s->h / 2), s->w, s->h};
        SDL_FreeSurface(s);
        if (t) {
            SDL_SetTextureAlphaMod(t, c.a);
            SDL_RenderCopy(renderer, t, nullptr, &d);
            SDL_DestroyTexture(t);
        }
    }
}

static void LogRuntimeMessage(const std::string &msg) {
#ifdef __EMSCRIPTEN__
    std::cout << msg << "\n";
#else
    std::ofstream f("pinball_runtime.log", std::ios::app);
    if (f) {
        f << msg << "\n";
    }
#endif
}

static int FatalStartup(const std::string &msg) {
    LogRuntimeMessage(msg);
#ifndef __EMSCRIPTEN__
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Pinball startup error", msg.c_str(), nullptr);
#endif
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    LogRuntimeMessage("pinball start");
#ifdef __EMSCRIPTEN__
    LogRuntimeMessage("cwd: / (web runtime)");
#else
    try {
        LogRuntimeMessage("cwd: " + fs::current_path().generic_string());
    } catch (const std::exception &ex) {
        LogRuntimeMessage(std::string("cwd read warning: ") + ex.what());
    }
#endif
    std::srand(static_cast<unsigned>(SDL_GetTicks()));
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        return FatalStartup(std::string("SDL_Init failed: ") + SDL_GetError());
    }
    if ((IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) & (IMG_INIT_PNG | IMG_INIT_JPG)) == 0) {
        std::cerr << "IMG_Init warning: " << IMG_GetError() << "\n";
    }
    if (TTF_Init() != 0) {
        std::cerr << "TTF_Init warning: " << TTF_GetError() << "\n";
    }
#if PINBALL_USE_MIXER
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
        std::cerr << "Mix_OpenAudio warning: " << Mix_GetError() << "\n";
    }
#else
    std::cerr << "SDL2_mixer header not found, running without audio.\n";
#endif

    SDL_Window *window = SDL_CreateWindow(
        "Advanced SDL2 Pinball", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, kScreenW, kScreenH, SDL_WINDOW_SHOWN);
    if (!window) {
        std::string err = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
        LogRuntimeMessage(err);
#if PINBALL_USE_MIXER
        Mix_CloseAudio();
#endif
        TTF_Quit();
        IMG_Quit();
        SDL_Quit();
        return FatalStartup(err);
    }
#ifdef __EMSCRIPTEN__
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
#else
    SDL_Renderer *renderer =
        SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
#endif
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!renderer) {
        std::string err = std::string("SDL_CreateRenderer failed: ") + SDL_GetError();
        LogRuntimeMessage(err);
        SDL_DestroyWindow(window);
#if PINBALL_USE_MIXER
        Mix_CloseAudio();
#endif
        TTF_Quit();
        IMG_Quit();
        SDL_Quit();
        return FatalStartup(err);
    }
    Synth synth;
    synth.Init();

    TTF_Font *font = nullptr;
    if (!font) {
        const std::array<std::string, 3> fallbackFonts = {
            "C:/Windows/Fonts/arial.ttf",
            "C:/Windows/Fonts/segoeui.ttf",
            "C:/Windows/Fonts/tahoma.ttf",
        };
        for (const auto &ff : fallbackFonts) {
            font = TTF_OpenFont(ff.c_str(), 28);
            if (font) {
                break;
            }
        }
    }

    Ball ball;
    std::vector<Ball> multiballs;
    multiballs.reserve(1024);
    std::vector<Vec2> trail;
    trail.reserve(20);

    std::vector<Segment> walls = {
        {{80.0f, 220.0f}, {80.0f, 1170.0f}, 0.76f},
        {{80.0f, 220.0f}, {86.0f, 170.0f}, 0.76f},
        {{86.0f, 170.0f}, {98.0f, 135.0f}, 0.76f},
        {{98.0f, 135.0f}, {118.0f, 108.0f}, 0.76f},
        {{118.0f, 108.0f}, {145.0f, 90.0f}, 0.76f},
        {{145.0f, 90.0f}, {180.0f, 82.0f}, 0.76f},
        {{180.0f, 82.0f}, {220.0f, 80.0f}, 0.76f},
        {{220.0f, 80.0f}, {740.0f, 80.0f}, 0.76f},
        {{740.0f, 80.0f}, {780.0f, 82.0f}, 0.76f},
        {{780.0f, 82.0f}, {815.0f, 90.0f}, 0.76f},
        {{815.0f, 90.0f}, {842.0f, 108.0f}, 0.76f},
        {{842.0f, 108.0f}, {862.0f, 135.0f}, 0.76f},
        {{862.0f, 135.0f}, {874.0f, 170.0f}, 0.76f},
        {{874.0f, 170.0f}, {880.0f, 220.0f}, 0.76f},
        {{880.0f, 220.0f}, {880.0f, 1220.0f}, 0.74f},
        {{836.0f, 900.0f}, {836.0f, 1220.0f}, 0.74f},
        {{836.0f, 900.0f}, {758.0f, 838.0f}, 0.86f},
        // Slingshot/inlane rails: guide toward center, avoid dead-end pockets.
        {{126.0f, 946.0f}, {238.0f, 1056.0f}, 0.82f},
        {{710.0f, 1056.0f}, {804.0f, 946.0f}, 0.82f},
        {{80.0f, 1170.0f}, {380.0f, 1220.0f}, 0.70f},
        {{580.0f, 1220.0f}, {880.0f, 1160.0f}, 0.70f},
    };

    std::vector<Bumper> bumpers = {
        {{260.0f, 340.0f}, 44.0f, 0.0f, 2100.0f, 10},
        {{490.0f, 290.0f}, 46.0f, 0.0f, 2300.0f, 25},
        {{720.0f, 380.0f}, 44.0f, 0.0f, 2200.0f, 10},
        {{490.0f, 550.0f}, 50.0f, 0.0f, 2500.0f, 15},
    };

    // Classic pinball hinge layout: pivots are outer/top corners of each flipper.
    Flipper left{{248.0f, 1072.0f}, 205.0f, 28.0f, 0.36f, -0.95f, 0.36f, true, false, 0.0f};
    Flipper right{{712.0f, 1072.0f}, 205.0f, 28.0f, 2.78f, 3.74f, 2.78f, false, false, 0.0f};
    Plunger plunger;

    int score = 0;
    int highScore = LoadHighScore();
    int ballsLeft = 3;
    float multiballSpawnTimer = 0.0f;
    int multiballWave = 0;
    bool gameOver = false;
    bool showHelpOverlay = true;
    bool running = true;
    bool launchReady = true;
    float wallSfxCooldown = 0.0f;
    std::vector<FloatingText> floatingTexts;
    floatingTexts.reserve(24);
    SaucerKicker saucer;
    Uint64 prevCounter = SDL_GetPerformanceCounter();
    double accumulator = 0.0;
    float fxTime = 0.0f;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = false;
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    running = false;
                } else if (e.key.keysym.sym == SDLK_LEFT) {
                    showHelpOverlay = false;
                    left.pressed = true;
                    if (!gameOver) {
                        synth.Play(790.0f, 0.045f, 0.22f, 0.18f, 0.06f);
                    }
                } else if (e.key.keysym.sym == SDLK_RIGHT) {
                    showHelpOverlay = false;
                    right.pressed = true;
                    if (!gameOver) {
                        synth.Play(760.0f, 0.045f, 0.22f, 0.18f, 0.06f);
                    }
                } else if (e.key.keysym.sym == SDLK_DOWN || e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN ||
                           e.key.keysym.sym == SDLK_KP_ENTER) {
                    showHelpOverlay = false;
                    if (gameOver && (e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)) {
                        ball.pos = {880.0f, 1100.0f};
                        ball.vel = {0.0f, 0.0f};
                        multiballs.clear();
                        score = 0;
                        ballsLeft = 3;
                        multiballSpawnTimer = 0.0f;
                        multiballWave = 0;
                        gameOver = false;
                        launchReady = true;
                        plunger.pullAmount = 0.0f;
                        synth.Play(520.0f, 0.11f, 0.22f, 0.25f, 0.10f);
                    } else if (!gameOver && launchReady) {
                        plunger.held = true;
                    }
                }
            } else if (e.type == SDL_KEYUP && !e.key.repeat) {
                if (e.key.keysym.sym == SDLK_LEFT) {
                    left.pressed = false;
                } else if (e.key.keysym.sym == SDLK_RIGHT) {
                    right.pressed = false;
                } else if (e.key.keysym.sym == SDLK_DOWN || e.key.keysym.sym == SDLK_SPACE || e.key.keysym.sym == SDLK_RETURN ||
                           e.key.keysym.sym == SDLK_KP_ENTER) {
                    plunger.held = false;
                    if (!gameOver && launchReady) {
                        float power = std::max(0.16f, plunger.pullAmount);
                        // Strong release with slight randomness gives classic pinball unpredictability.
                        ball.vel.y = -(900.0f + 1550.0f * power);
                        ball.vel.x = -220.0f + (static_cast<float>(std::rand() % 120) - 60.0f);
                        // Lane clear: if extra balls are stacked in shooter lane, plunger blast pushes them back into play.
                        for (auto &mb : multiballs) {
                            if (mb.pos.x > 836.0f && mb.pos.y > 860.0f) {
                                float jitter = static_cast<float>(std::rand() % 120) - 60.0f;
                                mb.vel.y = std::min(mb.vel.y, -(980.0f + 1150.0f * power));
                                mb.vel.x -= (220.0f + jitter * 0.35f);
                                mb.pos.x = std::max(822.0f, mb.pos.x - 4.0f);
                            }
                        }
                        plunger.pullAmount = 0.0f;
                        launchReady = false;
                        synth.Play(210.0f + power * 130.0f, 0.15f, 0.26f, 0.55f, 0.18f);
                    }
                }
            }
        }

        Uint64 currentCounter = SDL_GetPerformanceCounter();
        double frameDt = static_cast<double>(currentCounter - prevCounter) / SDL_GetPerformanceFrequency();
        prevCounter = currentCounter;
        frameDt = std::min(frameDt, 0.1);
        accumulator += frameDt;

        while (accumulator >= kFixedDt) {
            fxTime += kFixedDt;
            UpdateFlipper(left, kFixedDt);
            UpdateFlipper(right, kFixedDt);
            UpdatePlunger(plunger, kFixedDt);
            wallSfxCooldown = std::max(0.0f, wallSfxCooldown - kFixedDt);
            saucer.recaptureCooldown = std::max(0.0f, saucer.recaptureCooldown - kFixedDt);

            bool mainCapturedBySaucer = saucer.captured && saucer.capturedIsMain;
            if (saucer.captured) {
                saucer.holdTimer -= kFixedDt;
                if (mainCapturedBySaucer) {
                    ball.pos = saucer.pos;
                    ball.vel = {0.0f, 0.0f};
                }
                if (saucer.holdTimer <= 0.0f) {
                    saucer.captured = false;
                    float a = (static_cast<float>(std::rand() % 1000) / 1000.0f) * 2.0f * kPi;
                    float speed = 860.0f + static_cast<float>(std::rand() % 520);
                    Vec2 kickDir{std::cos(a), -std::abs(std::sin(a))};
                    if (saucer.capturedIsMain) {
                        ball.pos = saucer.pos + kickDir * (saucer.radius + ball.radius + 6.0f);
                        ball.vel = kickDir * speed;
                    } else {
                        saucer.capturedBall.pos = saucer.pos + kickDir * (saucer.radius + saucer.capturedBall.radius + 6.0f);
                        saucer.capturedBall.vel = kickDir * speed;
                        saucer.capturedBall.squash = 0.0f;
                        multiballs.push_back(saucer.capturedBall);
                    }
                    saucer.recaptureCooldown = 0.55f;
                    synth.Play(300.0f + static_cast<float>(std::rand() % 140), 0.16f, 0.32f, 0.62f, 0.24f);
                    floatingTexts.push_back({{saucer.pos.x, saucer.pos.y - 34.0f}, {0.0f, -26.0f}, "KICK!", 0.65f, 0.65f});
                    mainCapturedBySaucer = false;
                }
            } else {
                mainCapturedBySaucer = false;
            }

            if (gameOver || launchReady || mainCapturedBySaucer) {
                if (launchReady || gameOver) {
                    ball.pos = {850.0f, plunger.yCurrent - 34.0f};
                    ball.vel = {0.0f, 0.0f};
                }
            } else {
                // Adaptive sub-stepping reduces tunneling through thin/angled walls at high speed.
                float travel = Length(ball.vel) * kFixedDt;
                int subSteps = std::clamp(static_cast<int>(std::ceil(travel / (ball.radius * 0.45f))), 1, 8);
                float subDt = kFixedDt / static_cast<float>(subSteps);

                for (int i = 0; i < subSteps; ++i) {
                    ball.vel.y += 980.0f * subDt;
                    ball.vel = ball.vel * std::pow(0.999f, 120.0f * subDt);
                if (ball.pos.x > 846.0f && ball.pos.y > 900.0f) {
                        // Prevent soft-locks in shooter lane by biasing ball back into playfield.
                        ball.vel.x -= 520.0f * subDt;
                    }
                    ball.pos += ball.vel * subDt;

                    float wallImpact = 0.0f;
                    bool anyWallHit = false;
                    for (auto &w : walls) {
                        anyWallHit = ResolveBallSegment(ball, w, &wallImpact) || anyWallHit;
                    }
                    if (anyWallHit && wallImpact > 140.0f && wallSfxCooldown <= 0.0f) {
                        float pitch = std::clamp(180.0f + wallImpact * 0.22f, 170.0f, 520.0f);
                        synth.Play(pitch, 0.07f, 0.15f, 0.22f, 0.05f);
                        wallSfxCooldown = 0.045f;
                    }
                    for (auto &b : bumpers) {
                        int scoreBefore = score;
                        if (ResolveBallBumper(ball, b, score)) {
                            synth.Play(440.0f + static_cast<float>(std::rand() % 220), 0.14f, 0.28f, 0.48f, 0.22f);
                            int delta = score - scoreBefore;
                            if (delta > 0) {
                                floatingTexts.push_back(
                                    {{b.pos.x, b.pos.y - b.radius - 12.0f}, {0.0f, -34.0f}, "+" + std::to_string(delta), 0.55f, 0.55f});
                            }
                        }
                        b.flash = std::max(0.0f, b.flash - subDt * 2.4f);
                    }
                    ResolveBallFlipper(ball, left);
                    ResolveBallFlipper(ball, right);

                    Vec2 toSaucer = ball.pos - saucer.pos;
                    if (!saucer.captured && saucer.recaptureCooldown <= 0.0f && Length(toSaucer) < (ball.radius + saucer.radius)) {
                        saucer.captured = true;
                        saucer.capturedIsMain = true;
                        saucer.holdTimer = 0.85f + static_cast<float>(std::rand() % 80) / 100.0f;
                        score += 40;
                        synth.Play(250.0f, 0.10f, 0.22f, 0.30f, 0.12f);
                        floatingTexts.push_back({{saucer.pos.x, saucer.pos.y - 34.0f}, {0.0f, -30.0f}, "+40", 0.62f, 0.62f});
                        break;
                    }
                }
            }

            if (!gameOver) {
                for (size_t mbi = 0; mbi < multiballs.size(); ++mbi) {
                    Ball &mb = multiballs[mbi];
                    float travel = Length(mb.vel) * kFixedDt;
                    int subSteps = std::clamp(static_cast<int>(std::ceil(travel / (mb.radius * 0.45f))), 1, 6);
                    float subDt = kFixedDt / static_cast<float>(subSteps);
                    for (int i = 0; i < subSteps; ++i) {
                        mb.vel.y += 980.0f * subDt;
                        mb.vel = mb.vel * std::pow(0.999f, 120.0f * subDt);
                        mb.pos += mb.vel * subDt;

                        float wallImpact = 0.0f;
                        for (auto &w : walls) {
                            ResolveBallSegment(mb, w, &wallImpact);
                        }
                        for (auto &b : bumpers) {
                            int scoreBefore = score;
                            if (ResolveBallBumper(mb, b, score)) {
                                synth.Play(420.0f + static_cast<float>(std::rand() % 180), 0.10f, 0.20f, 0.36f, 0.16f);
                                int delta = score - scoreBefore;
                                if (delta > 0) {
                                    floatingTexts.push_back({{b.pos.x, b.pos.y - b.radius - 12.0f},
                                                             {0.0f, -34.0f},
                                                             "+" + std::to_string(delta),
                                                             0.45f,
                                                             0.45f});
                                }
                            }
                            b.flash = std::max(0.0f, b.flash - subDt * 2.4f);
                        }
                        ResolveBallFlipper(mb, left);
                        ResolveBallFlipper(mb, right);

                        Vec2 toSaucerMb = mb.pos - saucer.pos;
                        if (!saucer.captured && saucer.recaptureCooldown <= 0.0f &&
                            Length(toSaucerMb) < (mb.radius + saucer.radius)) {
                            saucer.captured = true;
                            saucer.capturedIsMain = false;
                            saucer.capturedBall = mb;
                            saucer.holdTimer = 0.80f + static_cast<float>(std::rand() % 90) / 100.0f;
                            score += 40;
                            synth.Play(250.0f, 0.10f, 0.22f, 0.30f, 0.12f);
                            floatingTexts.push_back({{saucer.pos.x, saucer.pos.y - 34.0f}, {0.0f, -30.0f}, "+40", 0.62f, 0.62f});
                            multiballs.erase(multiballs.begin() + mbi);
                            --mbi;
                            break;
                        }
                    }
                }

                if (!launchReady && !mainCapturedBySaucer) {
                    for (auto &mb : multiballs) {
                        ResolveBallBall(ball, mb);
                    }
                    for (size_t i = 0; i < multiballs.size(); ++i) {
                        for (size_t j = i + 1; j < multiballs.size(); ++j) {
                            ResolveBallBall(multiballs[i], multiballs[j]);
                        }
                    }
                }
                for (auto &mb : multiballs) {
                    if (mb.ttl >= 0.0f) {
                        mb.ttl -= kFixedDt;
                    }
                }
                multiballs.erase(std::remove_if(multiballs.begin(), multiballs.end(),
                                                [](const Ball &b) {
                                                    bool drained = b.pos.y > kScreenH + 60.0f;
                                                    bool expired = (b.ttl >= 0.0f && b.ttl <= 0.0f);
                                                    return drained || expired;
                                                }),
                                 multiballs.end());

                bool tableActive = (!launchReady && !mainCapturedBySaucer) || !multiballs.empty();
                if (tableActive) {
                    multiballSpawnTimer += kFixedDt;
                    while (multiballSpawnTimer >= 5.0f) {
                        multiballSpawnTimer -= 5.0f;
                        int spawnCount = std::min(12 * (multiballWave + 1), kMaxSpawnPerWave);
                        Vec2 anchor{500.0f, 760.0f};
                        Vec2 baseVel{0.0f, -180.0f};
                        if (!launchReady && !mainCapturedBySaucer) {
                            anchor = ball.pos;
                            baseVel = ball.vel;
                        } else if (!multiballs.empty()) {
                            anchor = multiballs.front().pos;
                            baseVel = multiballs.front().vel;
                        }
                        for (int i = 0; i < spawnCount && static_cast<int>(multiballs.size()) < kMaxMultiballs; ++i) {
                            float a = (static_cast<float>(std::rand() % 1000) / 1000.0f) * 2.0f * kPi;
                            float speed = 560.0f + static_cast<float>(std::rand() % 460);
                            Ball nb;
                            nb.pos = anchor + Vec2{std::cos(a) * 14.0f, std::sin(a) * 14.0f};
                            nb.vel = baseVel + Vec2{std::cos(a) * speed, std::sin(a) * speed};
                            nb.radius = ball.radius;
                            nb.squash = 0.0f;
                            nb.ttl = 2.0f + static_cast<float>(std::rand() % 300) / 100.0f;
                            nb.maxTtl = nb.ttl;
                            multiballs.push_back(nb);
                        }
                        floatingTexts.push_back({{500.0f, 86.0f},
                                                 {0.0f, -20.0f},
                                                 "+MULTI x" + std::to_string(spawnCount),
                                                 1.0f,
                                                 1.0f});
                        synth.Play(700.0f + 45.0f * spawnCount, 0.12f, 0.28f, 0.42f, 0.16f);
                        multiballWave += 1;
                    }
                } else {
                    multiballSpawnTimer = 0.0f;
                }
            }
            if (launchReady) {
                for (auto &b : bumpers) {
                    b.flash = std::max(0.0f, b.flash - kFixedDt * 2.4f);
                }
            }

            if (ball.pos.y > kScreenH + 60.0f) {
                ball.pos = {850.0f, 1100.0f};
                ball.vel = {0.0f, 0.0f};
                ballsLeft -= 1;
                if (ballsLeft > 0) {
                    launchReady = true;
                    plunger.pullAmount = 0.0f;
                    synth.Play(180.0f, 0.22f, 0.22f, 0.42f, 0.16f);
                } else {
                    launchReady = true;
                    gameOver = true;
                    multiballs.clear();
                    saucer.captured = false;
                    plunger.pullAmount = 0.0f;
                    if (score > highScore) {
                        highScore = score;
                        SaveHighScore(highScore);
                    }
                    synth.Play(260.0f, 0.22f, 0.25f, 0.35f, 0.15f);
                    synth.Play(210.0f, 0.25f, 0.24f, 0.30f, 0.12f);
                    synth.Play(170.0f, 0.30f, 0.23f, 0.24f, 0.10f);
                    synth.Play(130.0f, 0.34f, 0.21f, 0.20f, 0.08f);
                }
            }

            // If ball gets trapped back in shooter lane during active play, recapture it for relaunch.
            if (!launchReady && ball.pos.x > 842.0f && ball.pos.y > 920.0f && std::abs(ball.vel.x) < 120.0f) {
                launchReady = true;
                plunger.pullAmount = 0.0f;
                ball.pos = {850.0f, std::clamp(ball.pos.y, 980.0f, 1140.0f)};
                ball.vel = {0.0f, 0.0f};
            }

            ball.squash = std::max(0.0f, ball.squash - kFixedDt * 1.8f);
            for (auto &ft : floatingTexts) {
                ft.ttl -= kFixedDt;
                ft.pos += ft.vel * kFixedDt;
                ft.vel.y -= 18.0f * kFixedDt;
            }
            floatingTexts.erase(std::remove_if(floatingTexts.begin(), floatingTexts.end(),
                                               [](const FloatingText &ft) { return ft.ttl <= 0.0f; }),
                               floatingTexts.end());

            trail.push_back(ball.pos);
            if (trail.size() > 20) {
                trail.erase(trail.begin());
            }

            accumulator -= kFixedDt;
        }

        SDL_SetRenderDrawColor(renderer, 9, 12, 22, 255);
        SDL_RenderClear(renderer);

        SDL_Rect bg{64, 64, 832, 1164};
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 24, 40, 62, 255);
        SDL_RenderFillRect(renderer, &bg);
        float fxPulse = std::clamp(ball.squash * 0.65f, 0.0f, 1.0f);
        for (const auto &b : bumpers) {
            fxPulse = std::max(fxPulse, b.flash);
        }
        DrawFancyTableFx(renderer, bg, fxTime, fxPulse);

        SDL_SetRenderDrawColor(renderer, 92, 132, 210, 255);
        for (const auto &w : walls) {
            SDL_RenderDrawLine(renderer, static_cast<int>(w.a.x), static_cast<int>(w.a.y), static_cast<int>(w.b.x),
                               static_cast<int>(w.b.y));
        }

        for (size_t i = 0; i < trail.size(); ++i) {
            float t = static_cast<float>(i + 1) / trail.size();
            Uint8 alpha = static_cast<Uint8>(t * 165);
            DrawFilledCircle(renderer, static_cast<int>(trail[i].x), static_cast<int>(trail[i].y), static_cast<int>(6 + 9 * t),
                             {255, 205, 120, alpha});
        }
        for (const auto &mb : multiballs) {
            DrawVelocityTail(renderer, mb, {120, 205, 255, 115});
        }

        for (auto &b : bumpers) {
            Uint8 flash = static_cast<Uint8>(120.0f + b.flash * 120.0f);
            DrawGlow(renderer, b.pos, b.radius * (0.75f + b.flash * 0.6f), {220, 220, 255, flash});
            DrawFilledCircle(renderer, static_cast<int>(b.pos.x), static_cast<int>(b.pos.y), static_cast<int>(b.radius),
                             {250, static_cast<Uint8>(180 + b.flash * 75), 96, 220});
        }

        DrawGlow(renderer, saucer.pos, saucer.radius * 0.95f, {155, 115, 255, 118});
        DrawFilledCircle(renderer, static_cast<int>(saucer.pos.x), static_cast<int>(saucer.pos.y), static_cast<int>(saucer.radius),
                         saucer.captured ? SDL_Color{220, 180, 255, 255} : SDL_Color{145, 110, 230, 245});
        DrawFilledCircle(renderer, static_cast<int>(saucer.pos.x), static_cast<int>(saucer.pos.y), static_cast<int>(saucer.radius * 0.45f),
                         {70, 48, 112, 255});

        DrawFlipper(renderer, left, {255, 175, 95, 255});
        DrawFlipper(renderer, right, {255, 175, 95, 255});

        SDL_Rect plungerTrack{846, static_cast<int>(plunger.yTop), 34, static_cast<int>(plunger.yBottom - plunger.yTop)};
        SDL_SetRenderDrawColor(renderer, 52, 65, 86, 255);
        SDL_RenderFillRect(renderer, &plungerTrack);
        SDL_Rect plungerRect{840, static_cast<int>(plunger.yCurrent - 28), 46, 56};
        SDL_SetRenderDrawColor(renderer, 188, 198, 220, 255);
        SDL_RenderFillRect(renderer, &plungerRect);

        float stretch = std::clamp(ball.squash, 0.0f, 1.0f);
        float sx = 1.0f + stretch * 0.38f;
        float sy = 1.0f - stretch * 0.25f;
        int rw = static_cast<int>(ball.radius * 2.0f * sx);
        int rh = static_cast<int>(ball.radius * 2.0f * sy);
        SDL_Rect ballRect{static_cast<int>(ball.pos.x - rw / 2), static_cast<int>(ball.pos.y - rh / 2), rw, rh};
        DrawGlow(renderer, ball.pos, ball.radius * 0.85f, {255, 205, 120, 145});
        (void)ballRect;
        DrawFilledCircle(renderer, static_cast<int>(ball.pos.x), static_cast<int>(ball.pos.y), static_cast<int>(ball.radius),
                         {255, 236, 200, 255});
        DrawFilledCircle(renderer, static_cast<int>(ball.pos.x), static_cast<int>(ball.pos.y), std::max(2, static_cast<int>(ball.radius * 0.26f)),
                         {255, 170, 70, 230});
        for (const auto &mb : multiballs) {
            float lifeAlpha = 1.0f;
            if (mb.maxTtl > 0.0f && mb.ttl >= 0.0f) {
                lifeAlpha = std::clamp(mb.ttl / mb.maxTtl, 0.0f, 1.0f);
            }
            DrawGlow(renderer, mb.pos, mb.radius * 0.6f, {90, 170, 255, static_cast<Uint8>(95.0f * lifeAlpha)});
            DrawFilledCircle(renderer, static_cast<int>(mb.pos.x), static_cast<int>(mb.pos.y), static_cast<int>(mb.radius),
                             {165, 214, 255, static_cast<Uint8>(228.0f * lifeAlpha)});
        }

        if (score > highScore) {
            highScore = score;
        }
        DrawFloatingTexts(renderer, font, floatingTexts);
        DrawHud(renderer, font, score, highScore, ballsLeft, gameOver, showHelpOverlay);
        SDL_RenderPresent(renderer);
#ifdef __EMSCRIPTEN__
        emscripten_sleep(0);
#endif
    }

    if (font) {
        TTF_CloseFont(font);
    }
    SaveHighScore(highScore);
    synth.Shutdown();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
#if PINBALL_USE_MIXER
    Mix_CloseAudio();
#endif
    TTF_Quit();
    IMG_Quit();
    SDL_Quit();
    return 0;
}
