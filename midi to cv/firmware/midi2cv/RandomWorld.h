#pragma once

#include <array>
#include <cmath>
#include <random>
#include <vector>

enum class PropKind { pitch = 0, adsr = 1, effect = 2 };

enum class ScaleMode : int
{
    Free = 0,
    Major,
    Minor,
    Dorian,
    Phrygian,
    Lydian,
    Mixolydian,
    Locrian,
    HarmonicMinor,
    MelodicMinor,
    MajorPentatonic,
    MinorPentatonic,
    Blues,
    WholeTone,
    Diminished,
    Chromatic,
    JustMajor,
    JustMinor,
    Edo24,
    Edo31,
    Edo48,
    Edo72,
    Count
};

const char* scaleName (int mode);

struct V2
{
    float x = 0.0f;
    float y = 0.0f;

    V2() = default;
    V2 (float x_, float y_) : x (x_), y (y_) {}

    V2 operator+ (const V2& o) const { return { x + o.x, y + o.y }; }
    V2 operator- (const V2& o) const { return { x - o.x, y - o.y }; }
    V2 operator* (float s) const { return { x * s, y * s }; }
    V2 operator/ (float s) const { return { x / s, y / s }; }
    V2& operator+= (const V2& o) { x += o.x; y += o.y; return *this; }
    V2& operator-= (const V2& o) { x -= o.x; y -= o.y; return *this; }
    V2& operator*= (float s) { x *= s; y *= s; return *this; }
    V2& operator/= (float s) { x /= s; y /= s; return *this; }
    V2 operator-() const { return { -x, -y }; }

    float getDistanceFrom (const V2& o) const
    {
        const auto dx = x - o.x;
        const auto dy = y - o.y;
        return std::sqrt (dx * dx + dy * dy);
    }

    float getDistanceFromOrigin() const { return std::sqrt (x * x + y * y); }
    float getDotProduct (const V2& o) const { return x * o.x + y * o.y; }
    V2 translated (float dx, float dy) const { return { x + dx, y + dy }; }
    V2 normalized() const
    {
        const auto l = getDistanceFromOrigin();
        return l > 1.0e-6f ? V2 { x / l, y / l } : V2 { 1.0f, 0.0f };
    }
};

struct Prop
{
    PropKind kind = PropKind::pitch;
    float value = 60.0f;
};

struct SoundEvent
{
    float note = 60.0f;
    float pan = 0.0f;
    float send = 0.0f;
    float velocity = 0.5f;
    float attack = 0.005f;
    float decay = 0.15f;
    float sustain = 0.6f;
    float release = 0.3f;
};

struct WorldSettings
{
    float ballScale = 0.55f;
    float ballSpeed = 1.0f;
    float speedShare = 1.0f;
    float speedStrength = 1.0f;
    float speedCap = 280.0f;
    float waveSpeed = 0.65f;
    float waveRange = 0.5f;
    float pitchRange = 4.0f;
    int scaleMode = 0;
    std::array<float, 3> ratio { 0.50f, 0.25f, 0.25f };
};

struct BallVis { V2 pos; float radius; int kind; float value, note, flash, phase; };
struct SegmentVis { V2 a, b; bool hasProp; int kind; float value, strength; };
struct FieldVis { V2 pos; int kind; float value, radius, strength; };
struct WaveVis { V2 pos; float radius, amp; int kind; };
struct BoosterVis { V2 pos; float radius, cooldown, phase; };
struct LinkVis { V2 a, b; float amount; };

struct Snapshot
{
    std::array<BallVis, 40> balls {};
    std::array<SegmentVis, 220> segments {};
    std::array<FieldVis, 50> fields {};
    std::array<WaveVis, 48> waves {};
    std::array<BoosterVis, 5> boosters {};
    std::array<LinkVis, 48> links {};
    int ballCount = 0;
    int segmentCount = 0;
    int fieldCount = 0;
    int waveCount = 0;
    int boosterCount = 0;
    int linkCount = 0;
    std::array<int, 3> counts { 0, 0, 0 };
    float averageSpeed = 0.0f;
    float elapsed = 0.0f;
    int shape = 0;
};

class World
{
public:
    static constexpr float fieldX = 0.0f;
    static constexpr float fieldY = 0.0f;
    static constexpr float fieldW = 240.0f;
    static constexpr float fieldH = 222.0f;

    World();

    void reset();
    void setShape (int newShape);
    void setFieldScale (float scale);
    void setBallScale (float scale);
    void setPitchRange (float octaves);
    void setScale (int mode);
    void setSettings (const WorldSettings& s) { settings = s; }
    const WorldSettings& getSettings() const { return settings; }
    void clearEffects();
    void addBall() { spawnBall(); }
    bool addBallAt (V2 pos, int kind = -1);
    void addBooster() { spawnBooster(); }

    void update (float dt);
    void fillSnapshot (Snapshot& snap) const;
    std::vector<SoundEvent>& getEvents() { return events; }
    int getBallCount() const { return (int) balls.size(); }
    int getBoosterCount() const { return (int) boosters.size(); }

    int shape = 0;
    float fieldScale = 1.0f;
    bool paused = false;

    V2 randomPoint (float margin = 0.0f) const;
    bool inside (V2 p, float margin = 0.0f) const;

private:
    struct Segment
    {
        V2 a, b, n, mid;
        float length = 0.0f;
        bool hasProp = false;
        Prop prop;
        float strength = 0.0f;
    };

    struct Ball
    {
        V2 pos, vel;
        float radius = 6.0f;
        float r0 = 6.0f;
        Prop prop;
        float note = 60.0f;
        float wallCd = 0.0f;
        float ballCd = 0.0f;
        float flash = 0.0f;
        std::array<float, 3> waveAmp { 0.0f, 0.0f, 0.0f };
        std::array<float, 3> waveTimer { 0.0f, 0.0f, 0.0f };
        float phase = 0.0f;
        int uid = 0;
    };

    struct Field
    {
        V2 pos;
        Prop prop;
        float radius = 100.0f;
        float strength = 1.0f;
        float age = 0.0f;
        float life = 6.0f;
    };

    struct Wave
    {
        V2 pos;
        float radius = 6.0f;
        float speed = 250.0f;
        float amp = 1.0f;
        Prop prop;
        int generation = 0;
        bool reflected = false;
        float age = 0.0f;
        std::vector<int> hitUids;
    };

    struct Booster
    {
        V2 pos;
        float radius = 13.0f;
        float cooldown = 0.0f;
        float phase = 0.0f;
    };

    struct Spark
    {
        V2 pos, vel;
        float life = 0.3f;
        float maxLife = 0.3f;
        PropKind kind = PropKind::pitch;
        float value = 60.0f;
    };

    struct Link
    {
        V2 a, b;
        float amount = 0.0f;
    };

    void buildBoundary();
    void spawnBall (int forcedKind = -1, V2 forcedPos = {});
    void ensureTypes();
    void spawnBooster (V2 forcedPos = {});
    void addField (V2 pos, const Prop& prop, float radius, float life = 6.0f);
    void addWave (V2 pos, const Prop& prop, float amp, int generation = 0);
    void addSparks (V2 pos, const Prop& prop, int count, float speed);
    void bounceOffWall (Ball& ball);
    void ensureInside (Ball& ball);
    void shareSpeeds (float dt);
    void rebalanceType (Ball& ball);
    void enforceTypePresence();
    void collideBalls();
    void updateWaves (float dt);

    V2 closestOnSegment (V2 p, const Segment& s) const;
    float influenceAt (V2 p, PropKind kind) const;
    std::array<float, 3> targetRatios() const;
    Prop randomProp (PropKind kind) const;
    float randomNote() const;
    float quantizeNote (float note) const;
    void rebuildScaleTable();
    void emitSound (const Ball& b, const Prop& oldProp, float oldNote, float impact);
    static int kindIndex (PropKind k) { return static_cast<int> (k); }

    std::vector<Segment> segments;
    std::vector<V2> boundaryPoints;
    std::vector<Ball> balls;
    std::vector<Field> fields;
    std::vector<Wave> waves;
    std::vector<Booster> boosters;
    std::vector<Spark> sparks;
    std::vector<Link> links;
    std::vector<SoundEvent> events;
    std::vector<float> scaleTable;

    WorldSettings settings;
    mutable std::mt19937 rng;
    float elapsed = 0.0f;
    float typeGuardTimer = 0.0f;
    float wallGraceTimer = 0.0f;
    int nextUid = 1;
};
