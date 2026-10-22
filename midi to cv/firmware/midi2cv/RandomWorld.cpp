#include "RandomWorld.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>


namespace
{
constexpr float twoPi = 6.283185307179586f;
constexpr float pi = 3.141592653589793f;
constexpr float halfPi = 1.5707963267948966f;

template <typename T> T jmax (T a, T b) { return a > b ? a : b; }
template <typename T> T jmin (T a, T b) { return a < b ? a : b; }
template <typename T> T jlimit (T lo, T hi, T v) { return v < lo ? lo : (v > hi ? hi : v); }

float clampf (float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

V2 normalize (V2 p)
{
    const auto len = p.getDistanceFromOrigin();
    return len > 1.0e-6f ? p / len : V2 (1.0f, 0.0f);
}

float baseRadius (PropKind kind)
{
    if (kind == PropKind::pitch) return 62.0f;
    if (kind == PropKind::adsr) return 50.0f;
    return 78.0f;
}

float fieldRadius (PropKind kind)
{
    return baseRadius (kind);
}

constexpr float pitchCenter = 60.0f;

float pitchHalfSpan (float octaves)
{
    return jmax (0.5f, octaves) * 6.0f;
}

const char* const scaleNames[] {
    "FREE", "MAJOR", "MINOR", "DORIAN", "PHRYGIAN", "LYDIAN", "MIXOLYDIAN",
    "LOCRIAN", "HARM MIN", "MEL MIN", "MAJ PENT", "MIN PENT", "BLUES",
    "WHOLE", "DIMINISHED", "CHROMATIC", "JUST MAJ", "JUST MIN",
    "24-EDO", "31-EDO", "48-EDO", "72-EDO"
};
}

const char* scaleName (int mode)
{
    const auto last = (int) ScaleMode::Count - 1;
    mode = mode < 0 ? 0 : (mode > last ? last : mode);
    return scaleNames[mode];
}

World::World()
{
    balls.reserve (32);
    fields.reserve (24);
    waves.reserve (32);
    events.reserve (64);
    sparks.reserve (96);
    links.reserve (48);
    rng.seed (0x9E3779B9u);
    reset();
}

void World::buildBoundary()
{
    const auto cx = fieldX + fieldW * 0.5f;
    const auto cy = fieldY + fieldH * 0.5f;
    const auto pad = 6.0f;
    const auto rx = fieldW * 0.5f - pad;
    const auto ry = fieldH * 0.5f - pad;

    std::vector<V2> base;

    if (shape == 0)
    {
        const auto r = jmin (rx, ry);
        constexpr int n = 96;
        for (int i = 0; i < n; ++i)
        {
            const auto a = twoPi * (float) i / (float) n;
            base.emplace_back (cx + r * std::cos (a), cy + r * std::sin (a));
        }
    }
    else if (shape == 2)
    {
        const auto erx = rx;
        constexpr int n = 96;
        for (int i = 0; i < n; ++i)
        {
            const auto a = twoPi * (float) i / (float) n;
            base.emplace_back (cx + erx * std::cos (a), cy + ry * std::sin (a));
        }
    }
    else if (shape == 1)
    {
        const auto hx = jmin (rx, ry * 2.3f);
        const V2 corners[4] {
            { cx - hx, cy - ry },
            { cx + hx, cy - ry },
            { cx + hx, cy + ry },
            { cx - hx, cy + ry }
        };

        for (int i = 0; i < 4; ++i)
        {
            const auto a = corners[i];
            const auto b = corners[(i + 1) % 4];
            const auto steps = jmax (2, (int) (a.getDistanceFrom (b) / 40.0f));
            for (int k = 0; k < steps; ++k)
                base.push_back (a + (b - a) * ((float) k / (float) steps));
        }
    }
    else
    {
        const auto r = jmin (rx, ry);
        constexpr int sides = 6;
        V2 verts[sides];
        for (int i = 0; i < sides; ++i)
        {
            const auto a = -halfPi + twoPi * (float) i / (float) sides;
            verts[i] = { cx + r * std::cos (a), cy + r * std::sin (a) };
        }

        for (int i = 0; i < sides; ++i)
        {
            const auto a = verts[i];
            const auto b = verts[(i + 1) % sides];
            const auto steps = jmax (2, (int) (a.getDistanceFrom (b) / 30.0f));
            for (int k = 0; k < steps; ++k)
                base.push_back (a + (b - a) * ((float) k / (float) steps));
        }
    }

    if (std::abs (fieldScale - 1.0f) > 1.0e-4f)
    {
        const V2 center (cx, cy);
        for (auto& p : base)
            p = center + (p - center) * fieldScale;
    }

    boundaryPoints = base;
    segments.clear();
    segments.reserve (base.size());

    V2 centroid;
    for (const auto& p : base)
        centroid += p;
    centroid /= (float) base.size();

    for (size_t i = 0; i < base.size(); ++i)
    {
        const auto a = base[i];
        const auto b = base[(i + 1) % base.size()];
        const auto edge = b - a;
        if (edge.getDistanceFromOrigin() < 1.0e-4f)
            continue;

        Segment s;
        s.a = a;
        s.b = b;
        s.mid = (a + b) * 0.5f;
        s.length = edge.getDistanceFromOrigin();
        s.n = normalize ({ -edge.y, edge.x });
        if (s.n.getDotProduct (centroid - a) < 0.0f)
            s.n = -s.n;
        segments.push_back (std::move (s));
    }
}

void World::reset()
{
    buildBoundary();
    balls.clear();
    fields.clear();
    waves.clear();
    boosters.clear();
    sparks.clear();
    links.clear();
    events.clear();
    elapsed = 0.0f;
    typeGuardTimer = 0.0f;
    nextUid = 1;

    for (int i = 0; i < 12; ++i)
        spawnBall();

    ensureTypes();

    for (int i = 0; i < 2; ++i)
        spawnBooster();
}

void World::setShape (int newShape)
{
    if (newShape == shape)
        return;

    shape = newShape;
    buildBoundary();
    fields.clear();
    waves.clear();
    sparks.clear();

    for (auto& b : balls)
        if (! inside (b.pos, b.radius + 1.0f))
            b.pos = randomPoint (b.radius + 4.0f);

    for (auto& bo : boosters)
        if (! inside (bo.pos, bo.radius + 4.0f))
            bo.pos = randomPoint (bo.radius + 10.0f);
}

void World::setFieldScale (float scale)
{
    const auto old = fieldScale;
    if (std::abs (scale - old) < 1.0e-4f)
        return;

    fieldScale = scale;
    const auto ratio = old > 0.0f ? scale / old : 1.0f;
    buildBoundary();
    fields.clear();
    waves.clear();
    sparks.clear();

    const V2 center (fieldX + fieldW * 0.5f, fieldY + fieldH * 0.5f);

    // Scale every object around the field center so the whole layout follows
    // the field instead of being re-randomized. If an object would end up
    // overlapping the wall (because its radius does not scale), pull it back
    // along the center->object ray; keep its direction so the distribution
    // still tracks the field. Binary search on the convex boundary is enough.
    auto clampInside = [this, &center] (V2 p, float margin)
    {
        if (inside (p, margin))
            return p;

        const auto dir = p - center;
        auto lo = 0.0f;
        auto hi = 1.0f;
        for (int i = 0; i < 18; ++i)
        {
            const auto mid = (lo + hi) * 0.5f;
            if (inside (center + dir * mid, margin))
                lo = mid;
            else
                hi = mid;
        }
        return center + dir * lo;
    };

    // Move the whole layout with the field, but keep ball/booster sizes and
    // velocities untouched so the playable area really changes. The wall
    // collision audio is muted briefly so clamping cannot sound like a hit.
    wallGraceTimer = 0.18f;

    for (auto& b : balls)
        b.pos = clampInside (center + (b.pos - center) * ratio, b.radius + 1.0f);

    for (auto& bo : boosters)
        bo.pos = clampInside (center + (bo.pos - center) * ratio, bo.radius + 1.0f);
}

void World::setBallScale (float scale)
{
    settings.ballScale = scale;
    for (auto& b : balls)
        b.radius = jmax (2.0f, b.r0 * scale);
}

void World::setPitchRange (float octaves)
{
    octaves = clampf (octaves, 0.5f, 8.0f);
    if (std::abs (octaves - settings.pitchRange) < 1.0e-4f)
        return;

    // Keep each note's position inside the old range and map it onto the new
    // range, so turning the knob spreads or squeezes the whole pitch layout
    // instead of re-randomizing it.
    const auto oldHalf = pitchHalfSpan (settings.pitchRange);
    const auto newHalf = pitchHalfSpan (octaves);
    const auto k = oldHalf > 1.0e-6f ? newHalf / oldHalf : 1.0f;

    settings.pitchRange = octaves;
    rebuildScaleTable();

    for (auto& b : balls)
    {
        b.note = pitchCenter + (b.note - pitchCenter) * k;

        if (b.prop.kind == PropKind::pitch)
        {
            b.prop.value = pitchCenter + (b.prop.value - pitchCenter) * k;
            b.note = b.prop.value;
        }

        if (settings.scaleMode != 0)
        {
            b.note = quantizeNote (b.note);
            if (b.prop.kind == PropKind::pitch)
                b.prop.value = b.note;
        }
    }
}

void World::setScale (int mode)
{
    const auto last = (int) ScaleMode::Count - 1;
    mode = mode < 0 ? 0 : (mode > last ? last : mode);
    if (mode == settings.scaleMode)
        return;

    settings.scaleMode = mode;
    rebuildScaleTable();

    if (scaleTable.empty())
        return;

    for (auto& b : balls)
    {
        b.note = quantizeNote (b.note);
        if (b.prop.kind == PropKind::pitch)
            b.prop.value = b.note;
    }
}

void World::rebuildScaleTable()
{
    scaleTable.clear();

    const auto mode = settings.scaleMode;
    if (mode <= 0)
        return;

    // All values are cents relative to the C4 center. The table always spans
    // the current pitch range, so the RANGE knob still defines the spread.
    const auto halfCents = pitchHalfSpan (settings.pitchRange) * 100.0f;

    auto addPeriodic = [this, halfCents] (std::initializer_list<float> semitones)
    {
        const auto minOct = (int) std::floor ((-halfCents) / 1200.0f) - 1;
        const auto maxOct = (int) std::ceil (halfCents / 1200.0f) + 1;
        for (int oct = minOct; oct <= maxOct; ++oct)
        {
            for (auto s : semitones)
            {
                const auto cents = (float) oct * 1200.0f + s * 100.0f;
                if (cents >= -halfCents - 1.0e-3f && cents <= halfCents + 1.0e-3f)
                    scaleTable.push_back (cents);
            }
        }
    };

    auto addEdo = [this, halfCents] (int divisions)
    {
        const auto step = 1200.0f / (float) divisions;
        const auto first = (int) std::ceil (-halfCents / step);
        const auto last = (int) std::floor (halfCents / step);
        for (int k = first; k <= last; ++k)
            scaleTable.push_back ((float) k * step);
    };

    switch (static_cast<ScaleMode> (mode))
    {
        case ScaleMode::Major:          addPeriodic ({ 0, 2, 4, 5, 7, 9, 11 }); break;
        case ScaleMode::Minor:          addPeriodic ({ 0, 2, 3, 5, 7, 8, 10 }); break;
        case ScaleMode::Dorian:         addPeriodic ({ 0, 2, 3, 5, 7, 9, 10 }); break;
        case ScaleMode::Phrygian:       addPeriodic ({ 0, 1, 3, 5, 7, 8, 10 }); break;
        case ScaleMode::Lydian:         addPeriodic ({ 0, 2, 4, 6, 7, 9, 11 }); break;
        case ScaleMode::Mixolydian:     addPeriodic ({ 0, 2, 4, 5, 7, 9, 10 }); break;
        case ScaleMode::Locrian:        addPeriodic ({ 0, 1, 3, 5, 6, 8, 10 }); break;
        case ScaleMode::HarmonicMinor:  addPeriodic ({ 0, 2, 3, 5, 7, 8, 11 }); break;
        case ScaleMode::MelodicMinor:   addPeriodic ({ 0, 2, 3, 5, 7, 9, 11 }); break;
        case ScaleMode::MajorPentatonic: addPeriodic ({ 0, 2, 4, 7, 9 }); break;
        case ScaleMode::MinorPentatonic: addPeriodic ({ 0, 3, 5, 7, 10 }); break;
        case ScaleMode::Blues:          addPeriodic ({ 0, 3, 5, 6, 7, 10 }); break;
        case ScaleMode::WholeTone:      addPeriodic ({ 0, 2, 4, 6, 8, 10 }); break;
        case ScaleMode::Diminished:     addPeriodic ({ 0, 2, 3, 5, 6, 8, 9, 11 }); break;
        case ScaleMode::Chromatic:      addPeriodic ({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }); break;
        case ScaleMode::JustMajor:      addPeriodic ({ 0.0f, 2.0391f, 3.8631f, 4.9804f, 7.0196f, 8.8436f, 10.8827f }); break;
        case ScaleMode::JustMinor:      addPeriodic ({ 0.0f, 2.0391f, 3.1564f, 4.9804f, 7.0196f, 8.1369f, 10.1760f }); break;
        case ScaleMode::Edo24:          addEdo (24); break;
        case ScaleMode::Edo31:          addEdo (31); break;
        case ScaleMode::Edo48:          addEdo (48); break;
        case ScaleMode::Edo72:          addEdo (72); break;
        default: break;
    }

    std::sort (scaleTable.begin(), scaleTable.end());
    scaleTable.erase (std::unique (scaleTable.begin(), scaleTable.end(),
                                   [] (float a, float b) { return std::abs (a - b) < 0.01f; }),
                      scaleTable.end());
}

float World::randomNote() const
{
    std::uniform_real_distribution<float> dist (0.0f, 1.0f);
    const auto half = pitchHalfSpan (settings.pitchRange);

    if (! scaleTable.empty())
    {
        auto index = (int) (dist (rng) * (float) scaleTable.size());
        index = index < 0 ? 0 : (index >= (int) scaleTable.size() ? (int) scaleTable.size() - 1 : index);
        return pitchCenter + scaleTable[(size_t) index] / 100.0f;
    }

    return pitchCenter + (dist (rng) * 2.0f - 1.0f) * half;
}

float World::quantizeNote (float note) const
{
    if (scaleTable.empty())
        return note;

    const auto cents = (note - pitchCenter) * 100.0f;
    const auto it = std::lower_bound (scaleTable.begin(), scaleTable.end(), cents);

    if (it == scaleTable.begin())
        return pitchCenter + scaleTable.front() / 100.0f;
    if (it == scaleTable.end())
        return pitchCenter + scaleTable.back() / 100.0f;

    const auto hi = *it;
    const auto lo = *(it - 1);
    const auto best = (cents - lo <= hi - cents) ? lo : hi;
    return pitchCenter + best / 100.0f;
}

void World::clearEffects()
{
    fields.clear();
    waves.clear();
    sparks.clear();
}

bool World::addBallAt (V2 pos, int kind)
{
    if (! inside (pos, 12.0f))
        return false;
    spawnBall (kind, pos);
    return true;
}

std::array<float, 3> World::targetRatios() const
{
    std::array<float, 3> r { settings.ratio[0], settings.ratio[1], settings.ratio[2] };
    const auto sum = r[0] + r[1] + r[2];
    if (sum <= 1.0e-9f)
        return { 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f };
    for (auto& v : r)
        v /= sum;
    return r;
}

Prop World::randomProp (PropKind kind) const
{
    std::uniform_real_distribution<float> dist (0.0f, 1.0f);
    if (kind == PropKind::pitch)
        return { kind, randomNote() };
    if (kind == PropKind::adsr)
        return { kind, 0.25f + dist (rng) * 0.70f };
    return { kind, 0.35f + dist (rng) * 0.60f };
}

void World::spawnBall (int forcedKind, V2 forcedPos)
{
    if ((int) balls.size() >= 32)
        return;

    const auto ratios = targetRatios();
    int kindIndex = forcedKind;
    if (kindIndex < 0)
    {
        std::discrete_distribution<int> pick (ratios.begin(), ratios.end());
        kindIndex = pick (rng);
    }

    const auto kind = static_cast<PropKind> (kindIndex);
    auto prop = randomProp (kind);

    Ball b;
    b.prop = prop;
    b.note = kind == PropKind::pitch ? prop.value : randomNote();
    b.pos = forcedPos.getDistanceFromOrigin() > 1.0f ? forcedPos : randomPoint (12.0f);
    std::uniform_real_distribution<float> angleDist (0.0f, twoPi);
    std::uniform_real_distribution<float> speedDist (50.0f, 120.0f);
    const auto angle = angleDist (rng);
    const auto speed = speedDist (rng);
    b.vel = { std::cos (angle) * speed, std::sin (angle) * speed };
    b.r0 = 6.0f + (float) rng() / (float) rng.max() * 2.5f;
    b.radius = b.r0 * settings.ballScale;
    b.phase = angleDist (rng);
    b.uid = nextUid++;
    balls.push_back (std::move (b));
}

void World::ensureTypes()
{
    for (int kind = 0; kind < 3; ++kind)
    {
        const auto has = std::any_of (balls.begin(), balls.end(),
                                      [kind] (const Ball& b) { return (int) b.prop.kind == kind; });
        if (! has)
            spawnBall (kind);
    }
}

void World::spawnBooster (V2 forcedPos)
{
    if ((int) boosters.size() >= 5)
        return;

    Booster bo;
    bo.pos = forcedPos.getDistanceFromOrigin() > 1.0f ? forcedPos : randomPoint (26.0f);
    std::uniform_real_distribution<float> dist (0.0f, twoPi);
    bo.phase = dist (rng);
    boosters.push_back (bo);
}

V2 World::randomPoint (float margin) const
{
    std::uniform_real_distribution<float> xd (fieldX, fieldX + fieldW);
    std::uniform_real_distribution<float> yd (fieldY, fieldY + fieldH);

    for (int i = 0; i < 200; ++i)
    {
        V2 p (xd (rng), yd (rng));
        if (inside (p, margin))
            return p;
    }
    return { fieldX + fieldW * 0.5f, fieldY + fieldH * 0.5f };
}

bool World::inside (V2 p, float margin) const
{
    for (const auto& s : segments)
        if (s.n.getDotProduct (p - s.a) < margin)
            return false;
    return true;
}

V2 World::closestOnSegment (V2 p, const Segment& s) const
{
    const auto ab = s.b - s.a;
    const auto denom = ab.getDotProduct (ab);
    if (denom < 1.0e-9f)
        return s.a;
    const auto t = clampf ((p - s.a).getDotProduct (ab) / denom, 0.0f, 1.0f);
    return s.a + ab * t;
}

void World::update (float dt)
{
    events.clear();

    if (paused)
        return;

    elapsed += dt;
    typeGuardTimer -= dt;
    wallGraceTimer = jmax (0.0f, wallGraceTimer - dt);
    if (typeGuardTimer <= 0.0f)
    {
        typeGuardTimer = 0.75f;
        enforceTypePresence();
    }

    for (auto& bo : boosters)
        bo.cooldown = jmax (0.0f, bo.cooldown - dt);

    for (auto& b : balls)
    {
        b.wallCd = jmax (0.0f, b.wallCd - dt);
        b.ballCd = jmax (0.0f, b.ballCd - dt);
        b.flash = jmax (0.0f, b.flash - dt);

        for (int k = 0; k < 3; ++k)
        {
            if (b.waveTimer[k] > 0.0f)
            {
                b.waveTimer[k] -= dt;
                b.waveAmp[k] *= std::exp (-dt / 0.65f);
                if (b.waveTimer[k] <= 0.0f || b.waveAmp[k] < 0.02f)
                {
                    b.waveTimer[k] = 0.0f;
                    b.waveAmp[k] = 0.0f;
                }
            }
        }

        const auto disp = b.vel * dt * settings.ballSpeed;
        const auto steps = jmax (1, (int) std::ceil (disp.getDistanceFromOrigin()
                                                           / jmax (3.0f, b.radius * 0.8f)));
        for (int s = 0; s < steps; ++s)
        {
            b.pos += disp / (float) steps;
            bounceOffWall (b);
        }

        for (auto& bo : boosters)
        {
            if (bo.cooldown > 0.0f)
                continue;

            const auto delta = b.pos - bo.pos;
            const auto d = delta.getDistanceFromOrigin();
            if (d < b.radius + bo.radius)
            {
                const auto n = normalize (delta);
                b.pos = bo.pos + n * (b.radius + bo.radius + 0.8f);
                b.vel += n * 95.0f;
                auto speed = b.vel.getDistanceFromOrigin();
                if (speed > 400.0f)
                    b.vel *= 400.0f / speed;
                else if (speed < 90.0f && speed > 1.0e-6f)
                    b.vel *= 90.0f / speed;

                bo.cooldown = 0.8f;
                b.flash = 0.2f;
                addSparks (b.pos, b.prop, 6, 130.0f);

                if (b.prop.kind != PropKind::effect)
                    emitSound (b, b.prop, b.note, 190.0f);
            }
        }
    }

    collideBalls();

    for (auto& b : balls)
        ensureInside (b);

    shareSpeeds (dt);

    for (auto& f : fields)
    {
        f.age += dt;
        f.strength = jmax (0.0f, 1.0f - f.age / f.life);
    }
    fields.erase (std::remove_if (fields.begin(), fields.end(),
                                  [] (const Field& f) { return f.strength <= 0.01f; }),
                  fields.end());

    for (auto& s : segments)
    {
        if (s.hasProp)
        {
            s.strength *= std::exp (-dt / 38.0f);
            if (s.strength < 0.02f)
            {
                s.hasProp = false;
                s.strength = 0.0f;
            }
        }
    }

    updateWaves (dt);

    for (auto& sp : sparks)
    {
        sp.life -= dt;
        sp.pos += sp.vel * dt;
        sp.vel *= std::exp (-dt * 2.2f);
    }
    sparks.erase (std::remove_if (sparks.begin(), sparks.end(),
                                  [] (const Spark& s) { return s.life <= 0.0f; }),
                  sparks.end());
}

void World::bounceOffWall (Ball& ball)
{
    Segment* best = nullptr;
    auto bestPen = 0.0f;
    V2 bestClosest;

    for (auto& s : segments)
    {
        const auto closest = closestOnSegment (ball.pos, s);
        const auto d = ball.pos.getDistanceFrom (closest);
        if (d < ball.radius)
        {
            const auto pen = ball.radius - d;
            if (pen > bestPen)
            {
                bestPen = pen;
                best = &s;
                bestClosest = closest;
            }
        }
    }

    if (best == nullptr)
        return;

    auto n = ball.pos.getDistanceFrom (bestClosest) > 1.0e-4f
                 ? normalize (ball.pos - bestClosest)
                 : best->n;

    ball.pos += n * (bestPen + 0.6f);
    const auto vn = ball.vel.getDotProduct (n);
    const auto resizing = wallGraceTimer > 0.0f;
    if (vn < 0.0f)
        ball.vel -= n * ((resizing ? 1.0f : 2.0f) * vn);

    if (resizing || ball.wallCd > 0.0f)
        return;

    ball.wallCd = 0.14f;

    const auto oldProp = ball.prop;
    const auto oldNote = ball.note;

    if (best->hasProp && best->strength > 0.05f)
    {
        ball.prop = best->prop;
        if (ball.prop.kind == PropKind::pitch)
            ball.note = ball.prop.value;
        best->prop = oldProp;
        best->strength = 1.0f;
    }
    else
    {
        best->hasProp = true;
        best->prop = oldProp;
        best->strength = 1.0f;
    }

    rebalanceType (ball);

    ball.flash = 0.22f;
    addSparks (bestClosest, oldProp, 8, 170.0f);
    addField (bestClosest, oldProp, fieldRadius (oldProp.kind), 7.5f);
    addWave (bestClosest, oldProp, oldProp.kind == PropKind::effect ? 1.0f : 0.55f);
    emitSound (ball, oldProp, oldNote, 140.0f);
}

void World::ensureInside (Ball& ball)
{
    const Segment* worst = nullptr;
    auto worstD = std::numeric_limits<float>::infinity();

    for (const auto& s : segments)
    {
        const auto d = s.n.getDotProduct (ball.pos - s.a);
        if (d < worstD)
        {
            worstD = d;
            worst = &s;
        }
    }

    if (worst == nullptr || worstD >= 0.0f)
        return;

    const auto closest = closestOnSegment (ball.pos, *worst);
    ball.pos = closest + worst->n * (ball.radius + 0.8f);
    const auto vn = ball.vel.getDotProduct (worst->n);
    if (vn < 0.0f)
        ball.vel -= worst->n * ((wallGraceTimer > 0.0f ? 1.0f : 2.0f) * vn);
}

void World::collideBalls()
{
    const auto n = balls.size();
    for (size_t i = 0; i < n; ++i)
    {
        auto& a = balls[i];
        for (size_t j = i + 1; j < n; ++j)
        {
            auto& b = balls[j];
            const auto delta = b.pos - a.pos;
            const auto dist = delta.getDistanceFromOrigin();
            const auto minDist = a.radius + b.radius;
            if (dist < 1.0e-6f || dist >= minDist)
                continue;

            const auto nrm = delta / dist;
            const auto overlap = minDist - dist;
            a.pos -= nrm * (overlap * 0.5f);
            b.pos += nrm * (overlap * 0.5f);

            const auto rel = b.vel - a.vel;
            const auto vn = rel.getDotProduct (nrm);
            if (vn < 0.0f)
            {
                const auto ma = jmax (1.0f, a.radius * a.radius);
                const auto mb = jmax (1.0f, b.radius * b.radius);
                const auto jimp = -2.0f * vn / (1.0f / ma + 1.0f / mb);
                a.vel -= nrm * (jimp / ma);
                b.vel += nrm * (jimp / mb);
            }

            if (a.ballCd <= 0.0f && b.ballCd <= 0.0f && std::abs (vn) > 25.0f)
            {
                a.ballCd = 0.12f;
                b.ballCd = 0.12f;
                const auto mid = (a.pos + b.pos) * 0.5f;
                const auto impact = std::abs (vn);
                a.flash = 0.18f;
                b.flash = 0.18f;

                addField (mid, a.prop, fieldRadius (a.prop.kind) * 0.55f, 4.0f);
                if (a.prop.kind != b.prop.kind)
                    addField (mid, b.prop, fieldRadius (b.prop.kind) * 0.5f, 3.5f);

                if (a.prop.kind == PropKind::effect)
                    addWave (mid, a.prop, 0.7f);
                else
                    addWave (mid, a.prop, 0.35f);

                if (b.prop.kind == PropKind::effect)
                    addWave (mid, b.prop, 0.7f);
                else
                    addWave (mid, b.prop, 0.35f);

                addSparks (mid, a.prop, 5, 120.0f);

                if (a.prop.kind != PropKind::effect)
                    emitSound (a, a.prop, a.note, impact);
                if (b.prop.kind != PropKind::effect)
                    emitSound (b, b.prop, b.note, impact);
            }
        }
    }
}

void World::shareSpeeds (float dt)
{
    links.clear();
    const auto n = balls.size();
    if (n < 2)
        return;

    const auto radius = 90.0f * settings.speedShare;

    for (size_t i = 0; i < n; ++i)
    {
        auto& a = balls[i];
        for (size_t j = i + 1; j < n; ++j)
        {
            auto& b = balls[j];
            const auto d = a.pos.getDistanceFrom (b.pos);
            if (d >= radius)
                continue;

            auto sa = a.vel.getDistanceFromOrigin();
            auto sb = b.vel.getDistanceFromOrigin();
            const auto diff = sa - sb;
            if (std::abs (diff) < 6.0f)
                continue;

            const auto falloff = std::pow (1.0f - d / radius, 1.2f);
            const auto transfer = std::abs (diff) * 0.5f
                                * jmin (1.0f, 2.0f * dt * settings.speedStrength)
                                * falloff;

            const auto dirA = sa > 1.0e-6f ? a.vel / sa : V2 (0.0f, 0.0f);
            const auto dirB = sb > 1.0e-6f ? b.vel / sb : V2 (0.0f, 0.0f);

            if (diff > 0.0f)
            {
                a.vel = dirA * jmax (0.0f, sa - transfer);
                b.vel = sb > 1.0e-6f ? dirB * (sb + transfer) : dirA * transfer;
            }
            else
            {
                b.vel = dirB * jmax (0.0f, sb - transfer);
                a.vel = sa > 1.0e-6f ? dirA * (sa + transfer) : dirB * transfer;
            }

            if (links.size() < 24)
                links.push_back ({ a.pos, b.pos, jmin (1.0f, transfer / 80.0f) });
        }
    }

    auto total = 0.0f;
    for (const auto& b : balls)
        total += b.vel.getDistanceFromOrigin();

    const auto maxTotal = settings.speedCap * (float) n;
    if (total > maxTotal && total > 1.0e-6f)
    {
        const auto k = maxTotal / total;
        for (auto& b : balls)
            b.vel *= k;
    }
}

void World::rebalanceType (Ball& ball)
{
    const auto n = (int) balls.size();
    if (n < 6)
        return;

    std::array<int, 3> counts { 0, 0, 0 };
    for (const auto& b : balls)
        counts[kindIndex (b.prop.kind)]++;

    const auto ratios = targetRatios();
    std::array<bool, 3> over { false, false, false };
    std::array<bool, 3> under { false, false, false };
    for (int i = 0; i < 3; ++i)
    {
        const auto desired = ratios[(size_t) i] * (float) n;
        if ((float) counts[(size_t) i] > desired + 0.4f)
            over[(size_t) i] = true;
        else if ((float) counts[(size_t) i] < desired - 0.4f)
            under[(size_t) i] = true;
    }

    const auto idx = kindIndex (ball.prop.kind);
    if (! over[(size_t) idx])
        return;

    auto anyUnder = false;
    for (auto u : under)
        anyUnder = anyUnder || u;
    if (! anyUnder)
        return;

    const auto surplus = (float) counts[(size_t) idx] - ratios[(size_t) idx] * (float) n;
    std::uniform_real_distribution<float> dist (0.0f, 1.0f);
    if (dist (rng) > jmin (0.75f, 0.18f * surplus))
        return;

    auto bestKind = -1;
    auto bestDeficit = -1.0e9f;
    for (int i = 0; i < 3; ++i)
    {
        if (! under[(size_t) i])
            continue;
        const auto deficit = ratios[(size_t) i] * (float) n - (float) counts[(size_t) i];
        if (deficit > bestDeficit)
        {
            bestDeficit = deficit;
            bestKind = i;
        }
    }

    if (bestKind < 0)
        return;

    ball.prop = randomProp (static_cast<PropKind> (bestKind));
    if (ball.prop.kind == PropKind::pitch)
        ball.note = ball.prop.value;
    addSparks (ball.pos, ball.prop, 4, 90.0f);
}

void World::enforceTypePresence()
{
    const auto n = (int) balls.size();
    if (n < 6)
        return;

    std::array<int, 3> counts { 0, 0, 0 };
    for (const auto& b : balls)
        counts[kindIndex (b.prop.kind)]++;

    std::vector<int> missing;
    for (int i = 0; i < 3; ++i)
        if (counts[(size_t) i] == 0)
            missing.push_back (i);

    if (missing.empty())
        return;

    const auto most = (int) std::distance (counts.begin(),
                                           std::max_element (counts.begin(), counts.end()));
    if (counts[(size_t) most] <= 1)
        return;

    const auto ratios = targetRatios();
    const auto target = *std::max_element (missing.begin(), missing.end(),
                                           [&ratios] (int a, int b) {
                                               return ratios[(size_t) a] < ratios[(size_t) b];
                                           });

    const auto amount = jmax (1, jmin (2, (int) std::round (ratios[(size_t) target] * (float) n * 0.4f)));

    std::vector<size_t> candidates;
    for (size_t i = 0; i < balls.size(); ++i)
        if (kindIndex (balls[i].prop.kind) == most)
            candidates.push_back (i);

    std::shuffle (candidates.begin(), candidates.end(), rng);
    for (int k = 0; k < amount && k < (int) candidates.size(); ++k)
    {
        auto& b = balls[candidates[(size_t) k]];
        b.prop = randomProp (static_cast<PropKind> (target));
        if (b.prop.kind == PropKind::pitch)
            b.note = b.prop.value;
        b.flash = 0.3f;
        addSparks (b.pos, b.prop, 8, 150.0f);
        addWave (b.pos, b.prop, 0.45f);
    }
}

void World::addField (V2 pos, const Prop& prop, float radius, float life)
{
    Field f;
    f.pos = pos;
    f.prop = prop;
    f.radius = jmax (24.0f, radius * settings.waveRange);
    f.life = life;
    fields.push_back (std::move (f));
    if (fields.size() > 20)
        fields.erase (fields.begin());
}

void World::addWave (V2 pos, const Prop& prop, float amp, int generation)
{
    if (waves.size() > 24 || amp < 0.06f)
        return;

    Wave w;
    w.pos = pos;
    w.prop = prop;
    w.amp = amp;
    w.generation = generation;
    w.speed = prop.kind == PropKind::effect ? 250.0f : 330.0f;
    waves.push_back (std::move (w));
}

void World::addSparks (V2 pos, const Prop& prop, int count, float speed)
{
    std::uniform_real_distribution<float> angleDist (0.0f, twoPi);
    std::uniform_real_distribution<float> speedDist (0.25f, 1.0f);
    std::uniform_real_distribution<float> lifeDist (0.18f, 0.45f);

    for (int i = 0; i < count; ++i)
    {
        Spark s;
        s.pos = pos;
        const auto a = angleDist (rng);
        s.vel = { std::cos (a), std::sin (a) };
        s.vel *= speed * speedDist (rng);
        s.life = s.maxLife = lifeDist (rng);
        s.kind = prop.kind;
        s.value = prop.value;
        sparks.push_back (std::move (s));
    }

    if (sparks.size() > 80)
        sparks.erase (sparks.begin(), sparks.begin() + (long) (sparks.size() - 80));
}

void World::updateWaves (float dt)
{
    struct PendingWave { V2 pos; Prop prop; float amp; int generation; };
    std::vector<PendingWave> pendingWaves;

    for (auto& w : waves)
    {
        w.age += dt;
        const auto step = w.speed * settings.waveSpeed * dt;
        w.radius += step;
        w.amp *= std::exp (-dt / 2.6f);

        for (auto& b : balls)
        {
            if (std::find (w.hitUids.begin(), w.hitUids.end(), b.uid) != w.hitUids.end())
                continue;

            const auto d = b.pos.getDistanceFrom (w.pos);
            if (std::abs (d - w.radius) < 11.0f + b.radius)
            {
                w.hitUids.push_back (b.uid);
                const auto idx = kindIndex (w.prop.kind);
                b.waveAmp[(size_t) idx] = jmax (b.waveAmp[(size_t) idx], w.amp * 0.75f);
                b.waveTimer[(size_t) idx] = 1.6f;
            }
        }

        if (! w.reflected && w.age > 0.1f && w.generation < 2)
        {
            std::vector<const Segment*> hits;
            for (const auto& s : segments)
            {
                const auto d = s.mid.getDistanceFrom (w.pos);
                if (std::abs (d - w.radius) < step + 3.0f)
                    hits.push_back (&s);
            }

            if (! hits.empty())
            {
                w.reflected = true;
                std::vector<float> chosen;
                for (const auto* s : hits)
                {
                    const auto ang = std::atan2 (s->mid.y - w.pos.y, s->mid.x - w.pos.x);
                    bool ok = true;
                    for (auto c : chosen)
                    {
                        auto delta = std::abs (std::fmod (ang - c + pi,
                                                          twoPi) - pi);
                        if (delta <= 1.35f)
                        {
                            ok = false;
                            break;
                        }
                    }
                    if (ok)
                    {
                        chosen.push_back (ang);
                        pendingWaves.push_back ({ s->mid, w.prop, w.amp * 0.52f, w.generation + 1 });
                        addSparks (s->mid, w.prop, 3, 70.0f);
                    }
                    if (chosen.size() >= 2)
                        break;
                }
            }
        }
    }

    for (const auto& pw : pendingWaves)
        addWave (pw.pos, pw.prop, pw.amp, pw.generation);

    const auto maxRadius = jmin (300.0f, 380.0f * settings.waveRange);
    waves.erase (std::remove_if (waves.begin(), waves.end(),
                                 [maxRadius] (const Wave& w) {
                                     return w.amp <= 0.04f || w.radius >= maxRadius;
                                 }),
                 waves.end());
}

float World::influenceAt (V2 p, PropKind kind) const
{
    auto best = 0.0f;
    for (const auto& f : fields)
    {
        if (f.prop.kind != kind)
            continue;
        const auto d = p.getDistanceFrom (f.pos);
        if (d < f.radius)
            best = jmax (best, f.strength * std::pow (1.0f - d / f.radius, 1.6f));
    }
    return best;
}

void World::emitSound (const Ball& b, const Prop& oldProp, float oldNote, float impact)
{
    if (oldProp.kind == PropKind::effect)
        return;

    const auto pan = clampf ((b.pos.x - fieldX) / fieldW * 2.0f - 1.0f, -1.0f, 1.0f);
    auto pitchInf = influenceAt (b.pos, PropKind::pitch);
    auto effectInf = influenceAt (b.pos, PropKind::effect);
    pitchInf = jmax (pitchInf, b.waveAmp[0]);
    effectInf = jmax (effectInf, b.waveAmp[2]);

    auto note = oldNote - pitchInf * 18.0f;

    if (! scaleTable.empty())
    {
        note = quantizeNote (note);
    }
    else
    {
        const auto half = pitchHalfSpan (settings.pitchRange);
        note = clampf (note, jmax (0.0f, pitchCenter - half),
                             jmin (127.0f, pitchCenter + half));
    }
    const auto send = clampf (0.08f + effectInf * 0.85f, 0.0f, 1.0f);
    const auto velocity = clampf (impact / 420.0f, 0.12f, 1.0f);

    SoundEvent ev;
    ev.note = note;
    ev.pan = pan;
    ev.send = send;
    ev.velocity = velocity;
    ev.attack = 0.005f;
    ev.decay = 0.12f;
    ev.sustain = 0.6f;
    ev.release = 0.25f;

    if (oldProp.kind == PropKind::adsr)
    {
        ev.decay = 0.06f + oldProp.value * 0.5f;
        ev.release = 0.10f + oldProp.value * 0.9f;
    }

    events.push_back (ev);
}

void World::fillSnapshot (Snapshot& snap) const
{
    snap.ballCount = jmin ((int) balls.size(), (int) snap.balls.size());
    for (int i = 0; i < snap.ballCount; ++i)
    {
        const auto& b = balls[(size_t) i];
        snap.balls[(size_t) i] = { b.pos, b.radius, (int) b.prop.kind, b.prop.value, b.note, b.flash, b.phase };
    }

    snap.segmentCount = jmin ((int) segments.size(), (int) snap.segments.size());
    for (int i = 0; i < snap.segmentCount; ++i)
    {
        const auto& s = segments[(size_t) i];
        snap.segments[(size_t) i] = { s.a, s.b, s.hasProp, (int) s.prop.kind, s.prop.value, s.strength };
    }

    snap.fieldCount = jmin ((int) fields.size(), (int) snap.fields.size());
    for (int i = 0; i < snap.fieldCount; ++i)
    {
        const auto& f = fields[(size_t) i];
        snap.fields[(size_t) i] = { f.pos, (int) f.prop.kind, f.prop.value, f.radius, f.strength };
    }

    snap.waveCount = jmin ((int) waves.size(), (int) snap.waves.size());
    for (int i = 0; i < snap.waveCount; ++i)
    {
        const auto& w = waves[(size_t) i];
        snap.waves[(size_t) i] = { w.pos, w.radius, w.amp, (int) w.prop.kind };
    }

    snap.boosterCount = jmin ((int) boosters.size(), (int) snap.boosters.size());
    for (int i = 0; i < snap.boosterCount; ++i)
    {
        const auto& bo = boosters[(size_t) i];
        snap.boosters[(size_t) i] = { bo.pos, bo.radius, bo.cooldown, bo.phase };
    }

    snap.linkCount = jmin ((int) links.size(), (int) snap.links.size());
    for (int i = 0; i < snap.linkCount; ++i)
    {
        const auto& l = links[(size_t) i];
        snap.links[(size_t) i] = { l.a, l.b, l.amount };
    }

    snap.counts = { 0, 0, 0 };
    auto totalSpeed = 0.0f;
    for (const auto& b : balls)
    {
        snap.counts[(size_t) kindIndex (b.prop.kind)]++;
        totalSpeed += b.vel.getDistanceFromOrigin();
    }
    snap.averageSpeed = balls.empty() ? 0.0f : totalSpeed / (float) balls.size();
    snap.elapsed = elapsed;
    snap.shape = shape;
}
