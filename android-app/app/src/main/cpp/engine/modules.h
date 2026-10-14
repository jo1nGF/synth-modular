#pragma once
// Módulos simples só para validar o motor. Os módulos "de verdade"
// (Plaits, Stages, ...) entram como adaptadores em adapters/.

#include "engine.h"

namespace mod {

// Clock: saída 0 = gate.  param0 = taxa (Hz), param1 = largura do pulso (0..1)
class Clock : public Module {
 public:
  Clock() : Module(0, 1) {
    setParam(0, 2.f);
    setParam(1, 0.5f);
  }
  void process(int n) override {
    float* o = out(0);
    const float inc = param(0) / sampleRate_;
    const float width = param(1);
    for (int i = 0; i < n; ++i) {
      phase_ += inc;
      phase_ -= std::floor(phase_);
      o[i] = phase_ < width ? 1.f : 0.f;
    }
  }

 private:
  float phase_ = 0.f;
};

// Seq4: entrada 0 = clock (gate), saída 0 = V/oct.
// param0..3 = notas em semitonas acima de C4.
class Seq4 : public Module {
 public:
  Seq4() : Module(1, 1) {}
  void process(int n) override {
    const float* g = in(0);
    float* o = out(0);
    for (int i = 0; i < n; ++i) {
      const bool high = g[i] > 0.5f;
      if (high && !prev_) step_ = (step_ + 1) & 3;
      prev_ = high;
      o[i] = param(step_) / 12.f;
    }
  }

 private:
  int step_ = -1 & 3;  // começa no último passo; o 1º pulso cai no passo 0
  bool prev_ = false;
};

// SineOsc: entrada 0 = V/oct, saída 0 = seno.
class SineOsc : public Module {
 public:
  SineOsc() : Module(1, 1) {}
  void process(int n) override {
    const float* v = in(0);
    float* o = out(0);
    const float inv = 1.f / sampleRate_;
    for (int i = 0; i < n; ++i) {
      phase_ += 261.6256f * std::exp2(v[i]) * inv;
      phase_ -= std::floor(phase_);
      o[i] = std::sin(2.f * kPi * phase_);
    }
  }

 private:
  float phase_ = 0.f;
};

// AdEnv: entrada 0 = gate, saída 0 = envelope 0..1.
// param0 = ataque (s), param1 = decaimento (s)
class AdEnv : public Module {
 public:
  AdEnv() : Module(1, 1) {
    setParam(0, 0.005f);
    setParam(1, 0.25f);
  }
  void process(int n) override {
    const float* g = in(0);
    float* o = out(0);
    const float atkStep = 1.f / (std::max(param(0), 0.0005f) * sampleRate_);
    const float tau = std::max(param(1), 0.005f) * 0.25f;
    const float decCoef = 1.f - std::exp(-1.f / (tau * sampleRate_));
    for (int i = 0; i < n; ++i) {
      const bool high = g[i] > 0.5f;
      if (high && !prev_) attack_ = true;
      prev_ = high;
      if (attack_) {
        level_ += atkStep;
        if (level_ >= 1.f) {
          level_ = 1.f;
          attack_ = false;
        }
      } else {
        level_ -= level_ * decCoef;
      }
      o[i] = level_;
    }
  }

 private:
  float level_ = 0.f;
  bool attack_ = false, prev_ = false;
};

// Vca: entradas 0 = sinal, 1 = CV; saída 0 = sinal * CV.
class Vca : public Module {
 public:
  Vca() : Module(2, 1) {}
  void process(int n) override {
    const float* a = in(0);
    const float* c = in(1);
    float* o = out(0);
    for (int i = 0; i < n; ++i) o[i] = a[i] * c[i];
  }
};

}  // namespace mod
