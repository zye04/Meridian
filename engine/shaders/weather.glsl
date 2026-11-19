// The weather at a world position, the same rule as Weather::stateAt: the
// state the air there carried when it crossed the front line, read from the
// history (travel relative to now, oldest first) and blended between
// samples. Upwind of the line: the current state; beyond the history: its
// oldest sample.

struct WeatherSample {
    float coverage;
    float type;
    float precipitation;
};

WeatherSample weatherAt(SceneBuffer s, vec3 world) {
    Weather w = s.weather;
    WeatherSample current = WeatherSample(w.coverage, w.type, w.precipitation);
    float downwind = dot(world.xz, s.wind.direction) + w.frontLineUpwind;
    if (downwind <= 0.0 || w.historyCount == 0u) {
        return current;
    }
    float travel = -downwind;
    WeatherHistorySample older = w.history.samples[0];
    if (travel <= older.travel) {
        return WeatherSample(older.coverage, older.type, older.precipitation);
    }
    // The newest point is the current state at travel 0.
    WeatherHistorySample newer = WeatherHistorySample(0.0, w.coverage, w.type, w.precipitation);
    uint last = w.historyCount - 1u;
    if (travel < w.history.samples[last].travel) {
        uint lo = 0u;
        uint hi = last;
        while (hi - lo > 1u) {
            uint mid = (lo + hi) / 2u;
            if (w.history.samples[mid].travel <= travel) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        older = w.history.samples[lo];
        newer = w.history.samples[hi];
    } else {
        older = w.history.samples[last];
    }
    float span = newer.travel - older.travel;
    float t = span > 0.0 ? clamp((travel - older.travel) / span, 0.0, 1.0) : 0.0;
    return WeatherSample(mix(older.coverage, newer.coverage, t), mix(older.type, newer.type, t),
                         mix(older.precipitation, newer.precipitation, t));
}
