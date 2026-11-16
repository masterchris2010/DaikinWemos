#pragma once

// Small online predictor. No Arduino, network or actuator dependencies.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace OnlineLearning {
static const unsigned Inputs = 12, Hidden = 8, QueueSize = 21;
static const uint32_t Format = 1;

inline float bound(float value, float low, float high) {
  return value < low ? low : value > high ? high : value;
}

struct Net {
  float weights[Inputs * Hidden];
  float biases[Hidden];
  float outputs[Hidden];
  float bias;

  float predict(const float *x, float *activations = nullptr) const {
    float total = bias;
    for (unsigned h = 0; h < Hidden; ++h) {
      float z = biases[h];
      for (unsigned i = 0; i < Inputs; ++i) z += weights[i * Hidden + h] * x[i];
      const float activation = std::tanh(z);
      if (activations) activations[h] = activation;
      total += outputs[h] * activation;
    }
    return 2.0f * std::tanh(total * .5f);
  }

  void train(const float *x, float label, float importance = 1) {
    float hidden[Hidden];
    const float prediction = predict(x, hidden);
    const float gradient = bound((prediction - label) * (1 - prediction * prediction * .25f), -.5f, .5f);
    const float rate = .005f * importance;
    for (unsigned h = 0; h < Hidden; ++h) {
      const float inner = gradient * outputs[h] * (1 - hidden[h] * hidden[h]);
      for (unsigned i = 0; i < Inputs; ++i) {
        float &weight = weights[i * Hidden + h];
        weight = bound(weight - rate * (inner * x[i] + .00001f * weight), -3, 3);
      }
      biases[h] = bound(biases[h] - rate * inner, -3, 3);
      outputs[h] = bound(outputs[h] - rate * (gradient * hidden[h] + .00001f * outputs[h]), -3, 3);
    }
    bias = bound(bias - rate * gradient, -3, 3);
  }

  bool valid() const {
    for (float x : weights) if (!std::isfinite(x) || std::fabs(x) > 3) return false;
    for (float x : biases) if (!std::isfinite(x) || std::fabs(x) > 3) return false;
    for (float x : outputs) if (!std::isfinite(x) || std::fabs(x) > 3) return false;
    return std::isfinite(bias) && std::fabs(bias) <= 3;
  }
};

struct Scores {
  uint32_t count;
  float network, persistence, trend, champion;
  void add(float n, float p, float t, float c, bool recent) {
    if (count < 1000000000UL) ++count;
    const float denominator = recent && count > 240 ? 240.0f : float(count);
    network += (n - network) / denominator;
    persistence += (p - persistence) / denominator;
    trend += (t - trend) / denominator;
    champion += (c - champion) / denominator;
  }
  bool valid() const {
    return count <= 1000000000UL && std::isfinite(network) && network >= 0 && network <= 10 &&
      std::isfinite(persistence) && persistence >= 0 && persistence <= 10 &&
      std::isfinite(trend) && trend >= 0 && trend <= 10 &&
      std::isfinite(champion) && champion >= 0 && champion <= 10;
  }
};

// All fields are 32-bit: checkpoint hashing does not depend on uninitialised padding.
struct Persist {
  Net candidate, trial, champion;
  Scores recentActive, recentOff, trialActive, trialOff;
  uint32_t enabled, accepted, offAccepted, trialRunning, generation;
  uint32_t observed, activeObserved, trained, trials, promotions, sinceTrial;
};

struct Limits {
  uint32_t trainActive = 2400;        // Twenty hours of eligible active examples.
  uint32_t trainTotal = 2400;
  uint32_t validationActive = 240;    // Two hours on a frozen, separate model.
  uint32_t validationTotal = 480;
};

class Engine {
 public:
  Persist state{};
  float candidatePrediction = NAN, acceptedPrediction = NAN;
  bool dirty = false;

  explicit Engine(Limits limits = Limits()) : limits_(limits) { reset(); }

  void reset() {
    std::memset(&state, 0, sizeof(state));
    state.enabled = 1;
    uint32_t seed = 0x584d21;
    for (float &weight : state.candidate.weights) {
      seed = seed * 1664525UL + 1013904223UL;
      weight = (float((seed >> 16) & 0xffff) / 65535.0f - .5f) * .3f;
    }
    forgetPending();
    signatureKnown_ = false;
    dirty = false;
  }

  static bool validState(const Persist &p) {
    return p.enabled <= 1 && p.accepted <= 1 && p.offAccepted <= 1 && p.trialRunning <= 1 &&
      p.candidate.valid() && p.trial.valid() && p.champion.valid() &&
      p.recentActive.valid() && p.recentOff.valid() && p.trialActive.valid() && p.trialOff.valid() &&
      p.activeObserved <= p.observed && p.trained <= p.observed;
  }

  bool restore(const Persist &p) {
    if (!validState(p)) return false;
    state = p;
    forgetPending();
    signatureKnown_ = false;
    dirty = false;
    return true;
  }

  void forgetPending() {
    for (Pending &entry : pending_) entry.valid = false;
    next_ = 0;
    haveSample_ = false;
    candidatePrediction = acceptedPrediction = NAN;
  }

  bool controls(uint32_t signature) {
    const bool changed = !signatureKnown_ || signature != signature_;
    if (changed) { forgetPending(); signature_ = signature; signatureKnown_ = true; }
    return changed;
  }

  void setEnabled(bool enabled) {
    state.enabled = enabled ? 1 : 0;
    forgetPending();
    dirty = true;
  }

  static bool normalise(const float *raw, float *x) {
    static const float means[Inputs] = {22,10,22,22,600,0,.5f,.5f,.5f,.5f,.5f,.5f};
    static const float scales[Inputs] = {10,20,10,10,800,2,.5f,.5f,.5f,.5f,.5f,.5f};
    static const float minimum[Inputs] = {-10,-40,10,-20,0,-5,0,0,0,0,0,0};
    static const float maximum[Inputs] = {55,65,35,60,2500,5,1,1,1,1,1,1};
    for (unsigned i = 0; i < Inputs; ++i) {
      if (!std::isfinite(raw[i]) || raw[i] < minimum[i] || raw[i] > maximum[i]) return false;
      x[i] = bound((raw[i] - means[i]) / scales[i], -3, 3);
    }
    return true;
  }

  bool sample(uint32_t now, uint32_t signature, const float *raw) {
    controls(signature);
    float x[Inputs];
    if (!normalise(raw, x)) { forgetPending(); return false; }
    if (haveSample_ && uint32_t(now - lastSample_) > 45000UL) forgetPending();
    if (haveSample_ && uint32_t(now - lastSample_) < 25000UL) return false;
    lastSample_ = now;
    haveSample_ = true;
    candidatePrediction = acceptedPrediction = NAN;
    if (state.enabled) {
      ++state.observed;
      if (raw[6] > .5f) ++state.activeObserved;
      for (Pending &entry : pending_) {
        if (!entry.valid || uint32_t(now - entry.at) < 600000UL) continue;
        entry.valid = false;
        if (uint32_t(now - entry.at) > 635000UL) continue;
        const float label = raw[0] - entry.room;
        if (!std::isfinite(label) || std::fabs(label) > 4) continue;
        Scores &live = entry.active ? state.recentActive : state.recentOff;
        live.add(std::fabs(entry.candidate - label), std::fabs(label),
                 std::fabs(entry.trend - label), std::fabs(entry.champion - label), true);
        // The frozen trial is judged on predictions made before the outcome existed.
        if (state.trialRunning && entry.generation == state.generation) {
          Scores &score = entry.active ? state.trialActive : state.trialOff;
          score.add(std::fabs(entry.trial - label), std::fabs(label),
                    std::fabs(entry.trend - label), std::fabs(entry.champion - label), false);
          finishTrial();
        }
        state.candidate.train(entry.x, label, entry.active ? 1.0f : .1f);
        ++state.trained;
        ++state.sinceTrial;
      }
      if (!state.trialRunning && state.recentActive.count >= limits_.trainActive &&
          state.trained >= limits_.trainTotal && state.sinceTrial >= limits_.validationTotal) {
        state.trial = state.candidate;
        state.trialActive = Scores{};
        state.trialOff = Scores{};
        state.trialRunning = 1;
        ++state.generation;
        state.sinceTrial = 0;
      }
      candidatePrediction = raw[0] + state.candidate.predict(x);
      Pending &entry = pending_[next_];
      std::memcpy(entry.x, x, sizeof(x));
      entry.at = now;
      entry.room = raw[0];
      entry.candidate = candidatePrediction - raw[0];
      entry.trend = bound(2 * raw[5], -4, 4);
      entry.trial = state.trialRunning ? state.trial.predict(x) : 0;
      entry.champion = state.accepted ? state.champion.predict(x) : 0;
      entry.generation = state.trialRunning ? state.generation : 0;
      entry.active = raw[6] > .5f;
      entry.valid = true;
      next_ = (next_ + 1) % QueueSize;
      dirty = true;
    }
    if (state.accepted && (raw[6] > .5f || state.offAccepted))
      acceptedPrediction = raw[0] + state.champion.predict(x);
    return true;
  }

 private:
  struct Pending {
    float x[Inputs], room, candidate, trial, champion, trend;
    uint32_t at, generation;
    bool active, valid;
  } pending_[QueueSize]{};
  Limits limits_;
  uint32_t signature_ = 0, lastSample_ = 0;
  unsigned next_ = 0;
  bool signatureKnown_ = false, haveSample_ = false;

  bool better(const Scores &score, bool compareChampion) const {
    return score.network < .9f * score.persistence && score.network < .9f * score.trend &&
      (!compareChampion || score.network < .9f * score.champion);
  }

  void finishTrial() {
    if (state.trialActive.count < limits_.validationActive ||
        state.trialActive.count + state.trialOff.count < limits_.validationTotal) return;
    ++state.trials;
    if (better(state.trialActive, state.accepted != 0)) {
      state.champion = state.trial;
      state.accepted = 1;
      state.offAccepted = state.trialOff.count >= limits_.validationActive && better(state.trialOff, state.offAccepted != 0);
      ++state.promotions;
    }
    state.trialRunning = 0;
    dirty = true;
  }
};
} // namespace OnlineLearning
