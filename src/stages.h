#pragma once
// Stages: gerador de segmentos no estilo do módulo Stages da Mutable Instruments,
// com os modos do firmware alternativo "multi-mode" do qiemem (Bryan Head).
//
// IMPLEMENTAÇÃO PRÓPRIA a partir do manual oficial e do README do firmware do qiemem
// (github.com/qiemem/eurorack, branch bipolar). Não é um port do código.
// Onde o README não dá a fórmula, a aproximação está marcada com [ASSUMIDO].
//
// Modos (param kMode), na ordem do firmware do qiemem:
//   0 gerador de segmentos (original + melhorias do qiemem e do firmware oficial 1.2)
//   1 gerador de segmentos avançado (segmento aleatório, slew subida/descida, atenuverter)
//   2 seis envelopes DAHDSR independentes
//   3 seis envelopes DAHDSR idênticos
//   4 oscilador harmônico (Ouroboros)   5 oscilador harmônico com controles alternativos
//
// Unidades: 1.0 = 8 V. Saídas em 0..1 (negativas em modos bipolares).
// Entradas : 0..5 = gate de cada segmento; 6..11 = CV de cada segmento.
// Saídas   : 0..5 = saída de cada segmento.
//
// Params por segmento i (0..5):
//   [ 0+i] tipo: 0 ramp, 1 step, 2 hold, 3 random (só no modo 1)
//   [ 6+i] slider 0..1      [12+i] pot 0..1
//   [18+i] início de loop   [24+i] fim de loop (início = fim: loop de 1 segmento)
//   [30+i] gate fantasma (trata a entrada como ligada)
//   [37+i] bipolar          [43+i] sem re-trigger (ramp)
//   [49+i] faixa 0 = baixo, 1 = meio (padrão), 2 = alto (tempo de ramp / frequência de LFO)
//   [55+i] quantização: 0 off, 1 cromática, 2 maior, 3 pentatônica (step/hold/random)
//   [61+i] LFO reseta no trigger em vez de seguir o clock
//   [36]   modo
//   [121]  faixa do oscilador raiz no Ouroboros: 0 = 2 min a 2 Hz, 1 = 0,125 a 32 Hz, 2 = 16 Hz a ~4 kHz (padrão)
//   [122+i] faixa de razões harmônicas da coluna i: 0 = 1/16..1, 1 = 1/4..8 (padrão), 2 = 1..16x
//   [134]  modo daltônico dos LEDs (brilho e piscar diferenciam o tipo)
//   [135]  DAHDSR independentes: envelope selecionado (0..5)
//   [128+i] forma de onda da coluna i: 0 seno, 1 triângulo, 2 quadrada, 3 serra, 4 pulso estreito, 5 pulso muito estreito
//   Ouroboros: coluna 0 = raiz (slider = afinação grossa, pot = fina, CV = V/oct na convenção do motor,
//   1.0 = 1 oitava, saída = mix); colunas 1..5 = parciais (saída individual).
//   [67 + e*9 + k] DAHDSR independente e (0..5): k 0..5 = delay, attack, hold, decay,
//                  sustain (nível), release; k 6..8 = forma do attack, decay, release

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "engine.h"

namespace mod {

class Stages : public Module {
 public:
  static constexpr int kN = 6;
  enum Type { kRamp = 0, kStep = 1, kHold = 2, kRandom = 3 };
  enum Mode { kSegmentGen = 0, kAdvanced = 1, kDahdsrIndependent = 2, kDahdsrIdentical = 3 };
  enum Scale { kNoQuant = 0, kChromatic = 1, kMajor = 2, kPentatonic = 3 };
  enum Param {
    kType = 0, kSlider = 6, kPot = 12, kLoopStart = 18, kLoopEnd = 24, kDummyGate = 30,
    kMode = 36, kBipolar = 37, kNoRetrig = 43, kRange = 49, kQuant = 55, kLfoReset = 61,
    kEnv = 67, kHarmRootRange = 121, kHarmRatioRange = 122, kWave = 128, kColorBlind = 134,
    kEnvSelected = 135, kNumParams = 136
  };

  Stages() : Module(2 * kN, kN, kNumParams, 2 * kN) {
    for (int i = 0; i < kN; ++i) {
      setParam(kType + i, kRamp);
      setParam(kSlider + i, 0.5f);
      setParam(kPot + i, 0.5f);
      setParam(kRange + i, 1.f);
      setParam(kHarmRatioRange + i, 1.f);
    }
    setParam(kHarmRootRange, 2.f);
    for (int e = 0; e < kN; ++e)
      for (int k = 6; k < 9; ++k) setParam(kEnv + e * 9 + k, 0.5f);  // formas lineares por padrão
  }

  void seed(uint32_t s) { rng_ = s ? s : 1u; }

  // ---- mapeamentos públicos (também usados nos testes) --------------------------
  static float expMap(float x, float lo, float hi) { return lo * std::pow(hi / lo, clamp01(x)); }
  // Tempo de ramp: baixo = rápido (1 ms a 2,2 s), meio = padrão (1 ms a 16 s),
  // alto = lento (16 s a 13 min). [README do qiemem]
  static float rampTime(int range, float x) {
    return range == 0 ? expMap(x, 0.001f, 2.2f) : range == 2 ? expMap(x, 16.f, 780.f) : expMap(x, 0.001f, 16.f);
  }
  // Frequência de LFO livre: lento 2 min a 2 Hz, padrão 0,125 a 32,7 Hz, rápido 8 Hz a ~2 kHz.
  static float lfoFreq(int range, float x) {
    return range == 0 ? expMap(x, 1.f / 120.f, 2.f) : range == 2 ? expMap(x, 8.f, 2000.f) : expMap(x, 0.125f, 32.7f);
  }
  static float lfoSliderForFreq(int range, float f) {
    const float lo = range == 0 ? 1.f / 120.f : range == 2 ? 8.f : 0.125f;
    const float hi = range == 0 ? 2.f : range == 2 ? 2000.f : 32.7f;
    return std::log(f / lo) / std::log(hi / lo);
  }
  // Duração de hold e outros tempos padrão: 1 ms a 16 s. [ASSUMIDO] mapeamento exponencial.
  static float sliderToTime(float x) { return rampTime(1, x); }
  static float timeToSlider(float s) { return std::log(std::max(s, 0.001f) / 0.001f) / std::log(16000.f); }
  // DAHDSR: durações de 0 a 10 s, slider embaixo = estágio desligado. [ASSUMIDO] curva cúbica.
  static float dahdsrTime(float x) { return x < 0.01f ? 0.f : 10.f * x * x * x; }
  static float dahdsrSlider(float seconds) { return std::cbrt(seconds / 10.f); }
  // Quantização em volts. Cromática, maior ou pentatônica maior; aceita volts negativos.
  static float quantizeVolts(float v, int scale) {
    static const int major[7] = {0, 2, 4, 5, 7, 9, 11}, penta[5] = {0, 2, 4, 7, 9};
    const float semis = v * 12.f;
    const float oct = std::floor(semis / 12.f);
    const float deg = semis - 12.f * oct;
    float best = std::round(deg);
    if (scale != kChromatic) {
      const int* t = scale == kMajor ? major : penta;
      const int n = scale == kMajor ? 7 : 5;
      float bd = 1e9f;
      for (int j = 0; j <= n; ++j) {
        const float c = j < n ? static_cast<float>(t[j]) : 12.f;
        if (std::fabs(deg - c) < bd) { bd = std::fabs(deg - c); best = c; }
      }
    }
    return (12.f * oct + best) / 12.f;
  }


  // ---- Ouroboros: mapeamentos públicos ---------------------------------------------
  // Razões por faixa (README do qiemem): alto 1x..16x; médio (padrão) 1/4,1/2,1,1.5,2,3,4,5,6,8; baixo 1/16..1.
  // Entre duas razões a frequência "gruda" na razão (zona de snap) e só então desliza. [ASSUMIDO] 30% de snap de cada lado.
  static float harmRatio(int range, float x) {
    static const float mid[10] = {0.25f, 0.5f, 1.f, 1.5f, 2.f, 3.f, 4.f, 5.f, 6.f, 8.f};
    float tab[16];
    int n = 16;
    if (range == 2) for (int j = 0; j < 16; ++j) tab[j] = static_cast<float>(j + 1);
    else if (range == 0) for (int j = 0; j < 16; ++j) tab[j] = 1.f / static_cast<float>(16 - j);
    else { n = 10; for (int j = 0; j < 10; ++j) tab[j] = mid[j]; }
    const float u = clamp01(x) * static_cast<float>(n - 1);
    const int j = std::min(n - 2, static_cast<int>(u));
    float t = clamp01((u - static_cast<float>(j) - 0.3f) / 0.4f);
    t = t * t * (3.f - 2.f * t);
    return std::exp(std::log(tab[j]) + (std::log(tab[j + 1]) - std::log(tab[j])) * t);
  }
  // Frequência da raiz. octOffset = afinação fina + CV V/oct (em oitavas).
  // Alto: C0 a C8 (16 Hz a ~4 kHz); médio: 0,125 a 32 Hz; baixo: 2 min a 2 Hz.
  static float harmRootFreq(int range, float coarse, float octOffset) {
    const float p = std::pow(2.f, octOffset), c = clamp01(coarse);
    if (range == 2) return 16.3516f * std::pow(2.f, 8.f * c) * p;
    if (range == 1) return 0.125f * std::pow(2.f, 8.f * c) * p;
    return (1.f / 120.f) * std::pow(240.f, c) * p;
  }
  static float polyBlep(float t, float dt) {
    if (t < dt) { t /= dt; return t + t - t * t - 1.f; }
    if (t > 1.f - dt) { t = (t - 1.f) / dt; return t * t + t + t + 1.f; }
    return 0.f;
  }
  static float pulseWave(float p, float dt, float duty) {  // pulso com bandlimiting PolyBLEP
    float v = p < duty ? 1.f : -1.f;
    v += polyBlep(p, dt);
    float q = p - duty;
    if (q < 0.f) q += 1.f;
    return v - polyBlep(q, dt);
  }
  static float harmWave(int w, float p, float dt) {
    switch (w) {
      case 0: return std::sin(2.f * kPi * p);
      case 1: return p < 0.25f ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
      case 2: return pulseWave(p, dt, 0.5f);
      case 3: return 2.f * p - 1.f - polyBlep(p, dt);
      case 4: return pulseWave(p, dt, 0.25f);  // [ASSUMIDO] larguras de pulso
      default: return pulseWave(p, dt, 0.1f);
    }
  }

  void prepare(float sampleRate, int block) override {
    Module::prepare(sampleRate, block);
    for (auto& d : cvDelay_) d.assign(kDelayLen, 0.f);
    mask_ = -1;
    for (auto& e : env_) e = Env();
    for (auto& h : harm_) h = HarmState();
  }

  void process(int n) override {
    ledT_ += static_cast<float>(n) / sampleRate_;
    const int mode = std::min(5, std::max(0, static_cast<int>(param(kMode) + 0.5f)));
    if (mode != lastMode_) { lastMode_ = mode; mask_ = -1; for (auto& e : env_) e = Env(); }
    if (mode == kDahdsrIndependent || mode == kDahdsrIdentical) { runDahdsr(n, mode == kDahdsrIdentical); dahdsrLeds(mode == kDahdsrIdentical, n); return; }
    if (mode >= 4) { runHarmonic(n, mode == 5); harmonicLeds(n); return; }
    advanced_ = (mode == kAdvanced);
    syncGroups();
    const float* gate[kN]; const float* cv[kN]; float* o[kN];
    for (int i = 0; i < kN; ++i) { gate[i] = in(i); cv[i] = in(kN + i); o[i] = out(i); }
    for (int i = 0; i < kN; ++i) { ledLoop_[i] = 0; ledFlag_[i] = false; }
    for (int g = 0; g < ngroups_; ++g) runGroup(groups_[g], gate, cv, o, n);
    segmentLeds(n);
  }

 private:
  static constexpr int kDelayLen = 16001;

  struct Group {
    int first = 0, last = 0;
    bool gated = false;
    int active = -1;
    float phase = 0.f, timer = 0.f, startLevel = 0.f, holdDur = 0.001f;
    float out = 0.f, target = 0.f;
    bool prevGate = false, accept = true;
    int holdCountdown = 0;
    float lfoPhase = 0.f, clockPeriod = 0.f, lfoPrev = 0.f, lfoNext = 0.f;
    bool wrapped = false;
    int sinceEdge = 0, edgeCount = 0;
    float cvPhase = 0.f;
    int cvWrite = 0;
    uint32_t reg = 0;
    int seqPos = 0, seqDir = 1, seqCounter = 0, inhibit = 0;
    bool seqStarted = false, resetHigh = false;
    float cx = -3.f, cy = 2.f, cz = 20.f;
  };
  struct HarmState { float phase = 0.f, strum = 0.f; bool prevGate = false; };
  struct Env { int phase = 0; float t = 0.f, level = 0.f, startLevel = 0.f; bool prevGate = false; };

  static float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
  static float clampSym(float x) { return x < -1.f ? -1.f : (x > 1.f ? 1.f : x); }
  float rand01() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
    return static_cast<float>(rng_ >> 8) * (1.f / 16777216.f);
  }
  static float warp(float t, float c) {  // [ASSUMIDO] <0.5 acelera, 0.5 linear, >0.5 desacelera
    const float d = (c - 0.5f) * 2.f, p = 1.f + 5.f * std::fabs(d);
    return d < 0.f ? std::pow(t, p) : 1.f - std::pow(1.f - t, p);
  }
  static float lfoShape(float pot, float ph) {  // [ASSUMIDO]
    const float x = clamp01(pot) * 3.f;
    if (x < 1.f) { const float s = 0.0001f + 0.4999f * x; return ph < s ? ph / s : 1.f - (ph - s) / (1.f - s); }
    if (x < 2.f) return 0.5f - 0.5f * std::cos(2.f * kPi * ph);
    const float f = 0.9f * std::min(x - 2.f, 1.f), edge = std::max(0.0001f, (1.f - f) * 0.5f);
    if (ph < edge) return ph / edge;
    if (ph < edge + f) return 1.f;
    if (ph < 2.f * edge + f) return 1.f - (ph - edge - f) / edge;
    return 0.f;
  }
  // Razões do LFO sincronizado (README do qiemem): lento, médio (padrão), rápido.
  static float clockRatio(int range, float x) {
    static const float slow[10] = {1.f/32, 1.f/16, 1.f/8, 1.f/7, 1.f/6, 1.f/5, 1.f/4, 1.f/3, 1.f/2, 1.f};
    static const float mid[7] = {1.f/4, 1.f/3, 1.f/2, 1.f, 2.f, 3.f, 4.f};
    static const float fast[10] = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 12.f, 16.f};
    const float* t = range == 0 ? slow : range == 2 ? fast : mid;
    const int n = range == 1 ? 7 : 10;
    return t[std::min(n - 1, static_cast<int>(clamp01(x) * n))];
  }
  static float randFreq(int range, float x) {  // LFO aleatório: 2 min–2 Hz / 8 s–32 Hz / 32 Hz–8 kHz
    return range == 0 ? expMap(x, 1.f / 120.f, 2.f) : range == 2 ? expMap(x, 32.f, 8000.f) : expMap(x, 0.125f, 32.f);
  }
  float glideCoef(float pot) const {  // [ASSUMIDO]
    const float tau = clamp01(pot) * clamp01(pot) * 2.f;
    return tau < 0.0005f ? 1.f : 1.f - std::exp(-1.f / (tau * sampleRate_));
  }
  static float polarOut(float u, bool bi) { return bi ? (u * 2.f - 1.f) * 0.625f : u; }  // bipolar = -5..+5 V

  bool bipolar(int i) const { return param(kBipolar + i) > 0.5f; }
  bool noRetrig(int i) const { return param(kNoRetrig + i) > 0.5f; }
  bool lfoReset(int i) const { return param(kLfoReset + i) > 0.5f; }
  int rangeOf(int i) const { return std::min(2, std::max(0, static_cast<int>(param(kRange + i) + 0.5f))); }
  int scaleOf(int i) const { return std::min(3, std::max(0, static_cast<int>(param(kQuant + i) + 0.5f))); }
  int typeOf(int i) const { return std::min(advanced_ ? 3 : 2, std::max(0, static_cast<int>(param(kType + i) + 0.5f))); }

  // Nível de um segmento step/hold: polaridade e quantização do firmware do qiemem.
  // Quantizado: o slider vale 0..2 V (±2 V se bipolar) somado ANTES de quantizar.
  float segLevel(int i, const float* const* cv, int k) const {
    const float s = param(kSlider + i);
    const int sc = scaleOf(i);
    if (sc == kNoQuant) return clampSym((bipolar(i) ? s * 2.f - 1.f : s) + cv[i][k]);
    const float volts = (bipolar(i) ? s * 4.f - 2.f : s * 2.f) + cv[i][k] * 8.f;
    return clampSym(quantizeVolts(volts, sc) / 8.f);
  }
  float quantOut(float v, int i) const { return scaleOf(i) == kNoQuant ? v : clampSym(quantizeVolts(v * 8.f, scaleOf(i)) / 8.f); }

  void syncGroups() {
    bool patched[kN]; int mask = 0, firstPatched = -1;
    for (int i = 0; i < kN; ++i) {
      patched[i] = inputConnected(i) || param(kDummyGate + i) > 0.5f;
      if (patched[i]) { mask |= 1 << i; if (firstPatched < 0) firstPatched = i; }
    }
    if (mask == mask_) return;
    mask_ = mask; ngroups_ = 0;
    const int limit = firstPatched < 0 ? kN : firstPatched;
    for (int i = 0; i < limit; ++i) addGroup(i, i, false);
    for (int i = limit; i < kN;) { int j = i + 1; while (j < kN && !patched[j]) ++j; addGroup(i, j - 1, true); i = j; }
  }
  void addGroup(int first, int last, bool gated) {
    Group g; g.first = first; g.last = last; g.gated = gated;
    g.reg = static_cast<uint32_t>(rand01() * 65535.f); g.lfoPrev = rand01(); g.lfoNext = rand01();
    groups_[ngroups_++] = g;
  }

  void runGroup(Group& G, const float* const* gate, const float* const* cv, float* const* o, int n) {
    int ls = -1, le = -1;
    for (int i = G.first; i <= G.last && ls < 0; ++i) if (param(kLoopStart + i) > 0.5f) ls = i;
    if (ls >= 0) for (int i = ls; i <= G.last; ++i) if (param(kLoopEnd + i) > 0.5f) { le = i; break; }
    const bool loop = ls >= 0 && le >= ls;
    int types[kN];
    for (int i = G.first; i <= G.last; ++i) types[i] = typeOf(i);
    const int size = G.last - G.first + 1;
    const bool ext = size >= 3 && isExtSeq(G, types);
    for (int i = G.first; i <= G.last; ++i) {
      ledType_[i] = types[i];
      ledFlag_[i] = bipolar(i) || (types[i] == kRamp && noRetrig(i));
    }
    if (loop) { if (ls == le) ledLoop_[ls] = 1; else { ledLoop_[ls] = 2; ledLoop_[le] = 3; } }
    for (int k = 0; k < n; ++k) {
      const bool gateHigh = G.gated && gate[G.first][k] > 0.5f;
      const bool rising = gateHigh && !G.prevGate;
      G.prevGate = gateHigh;
      if (size == 1) runSingle(G, types[G.first], loop, gateHigh, rising, cv, k);
      else if (ext) runExtendedSeq(G, types, loop, ls, le, rising, cv, k);
      else runMulti(G, types, loop, ls, le, gateHigh, rising, cv, k);
      o[G.first][k] = G.out;
      for (int j = G.first + 1; j <= G.last; ++j) o[j][k] = activity(G, j, types[j]);
    }
  }
  float activity(const Group& G, int j, int type) const {
    if (G.active != j) return 0.f;
    if (type == kRamp) return 1.f - clamp01(G.phase);
    if (type == kHold) return 1.f - clamp01(G.timer / std::max(G.holdDur, 0.0001f));
    return 1.f;
  }

  // Avança a fase de um LFO (livre, resetável ou sincronizado ao clock).
  void advancePhase(Group& G, int i, bool rising, const float* const* cv, int k, bool randomLfo) {
    const int rng = rangeOf(i);
    const float x = param(kSlider + i) + cv[i][k];
    float inc;
    if (!G.gated || lfoReset(i)) {
      inc = (randomLfo ? randFreq(rng, x) : lfoFreq(rng, x)) / sampleRate_;
      if (G.gated && rising) G.lfoPhase = 0.f;
    } else {
      ++G.sinceEdge;
      const float r = clockRatio(rng, x);
      if (rising) {
        if (G.sinceEdge > 8) G.clockPeriod = static_cast<float>(G.sinceEdge);
        G.sinceEdge = 0; ++G.edgeCount;
        if (G.clockPeriod > 0.f && (r >= 1.f || (G.edgeCount % static_cast<int>(1.f / r + 0.5f)) == 0)) G.lfoPhase = 0.f;
      }
      inc = G.clockPeriod > 0.f ? r / G.clockPeriod : 0.f;
    }
    G.lfoPhase += inc;
    G.wrapped = G.lfoPhase >= 1.f;
    if (G.wrapped) G.lfoPhase -= std::floor(G.lfoPhase);
  }

  void stepRegister(Group& G, int steps, float pFlip) {  // Turing Machine: registrador de 16 bits
    const uint32_t m = (1u << steps) - 1u, r = G.reg & m;
    uint32_t bit = (r >> (steps - 1)) & 1u;
    if (rand01() < pFlip) bit ^= 1u;
    G.reg = ((r << 1) | bit) & m;
  }
  float regValue(const Group& G, int i, const float* const* cv, int k) const {
    const int steps = 1 + static_cast<int>(clamp01(param(kPot + i)) * 15.f + 0.5f);
    const uint32_t m = (1u << steps) - 1u;
    (void)cv; (void)k;
    return quantOut(polarOut(static_cast<float>(G.reg & m) / static_cast<float>(m), bipolar(i)), i);
  }

  // ---- Segmento isolado ----------------------------------------------------------
  void runSingle(Group& G, int type, bool loop, bool gateHigh, bool rising, const float* const* cv, int k) {
    const int i = G.first;
    const float pot = clamp01(param(kPot + i));
    switch (type) {
      case kRamp:
        if (loop) {  // LFO
          advancePhase(G, i, rising, cv, k, false);
          G.out = (G.gated && !lfoReset(i) && G.clockPeriod <= 0.f) ? 0.f : polarOut(lfoShape(pot, G.lfoPhase), bipolar(i));
        } else if (G.gated) {  // decaimento: vai a 8 V e cai a 0
          if (rising && !(G.active == i && noRetrig(i))) { G.active = i; G.phase = 0.f; G.out = 1.f; }
          if (G.active == i) {
            G.phase += 1.f / (rampTime(rangeOf(i), param(kSlider + i) + cv[i][k]) * sampleRate_);
            if (G.phase >= 1.f) { G.phase = 1.f; G.out = 0.f; G.active = -1; }
            else G.out = 1.f - warp(G.phase, pot);
          }
        } else if (advanced_) {  // slew com subida e descida independentes (CV = alvo)
          float target = cv[i][k];
          if (!bipolar(i)) target = std::fabs(target);  // retificação de onda completa (modo unipolar)
          const float t = rampTime(rangeOf(i), target > G.out ? pot : param(kSlider + i));  // pot = subida, slider = descida
          G.out += (target - G.out) * (1.f - std::exp(-1.f / (t * sampleRate_)));
        } else {
          G.out = 0.f;  // [ASSUMIDO] rampa livre sem loop fica ociosa no modo original
        }
        break;

      case kStep:
        if (advanced_ && loop) {  // atenuverter: atenua em vez de deslizar
          const float s = param(kSlider + i), att = bipolar(i) ? pot * 2.f - 1.f : pot;
          const float off = clampSym(bipolar(i) ? s * 2.f - 1.f : s);
          if (!G.gated) {
            G.out = quantOut(clampSym(att * cv[i][k] + off), i);
          } else {
            if (rising) G.holdCountdown = std::max(1, static_cast<int>(0.002f * sampleRate_));
            if (G.holdCountdown > 0 && --G.holdCountdown == 0) G.target = cv[i][k];
            G.out = quantOut(clampSym(att * G.target + off), i);
          }
          break;
        }
        if (!G.gated) {
          G.target = segLevel(i, cv, k);  // acompanha slider/CV, sujeito ao glide
        } else {
          if (rising) G.holdCountdown = std::max(1, static_cast<int>(0.002f * sampleRate_));  // atraso de 2 ms do S&H
          if (G.holdCountdown > 0 && --G.holdCountdown == 0) G.target = segLevel(i, cv, k);
        }
        G.out += (G.target - G.out) * glideCoef(pot);
        break;

      case kHold:
        if (!G.gated) {  // atraso de CV
          G.cvPhase += 1000.f / sampleRate_;
          if (G.cvPhase >= 1.f) { G.cvPhase -= 1.f; G.cvWrite = (G.cvWrite + 1) % kDelayLen; cvDelay_[i][G.cvWrite] = segLevel(i, cv, k); }
          const int steps = std::min(kDelayLen - 1, static_cast<int>(sliderToTime(pot) * 1000.f));
          G.out = cvDelay_[i][(G.cvWrite - steps + kDelayLen) % kDelayLen];
        } else if (loop) {  // gerador de gate; o pot é a probabilidade de disparo (firmware oficial 1.2)
          if (rising) G.accept = rand01() < pot;
          G.out = (gateHigh && G.accept) ? segLevel(i, cv, k) : 0.f;
        } else {  // pulso temporizado
          if (rising) { G.active = i; G.timer = 0.f; G.holdDur = sliderToTime(pot); }
          if (G.active == i) {
            G.timer += 1.f / sampleRate_;
            if (G.timer >= G.holdDur) { G.active = -1; G.out = 0.f; } else G.out = segLevel(i, cv, k);
          } else G.out = 0.f;
        }
        break;

      default:  // kRandom (modo avançado)
        runRandomSingle(G, loop, rising, cv, k);
        break;
    }
  }

  void runRandomSingle(Group& G, bool loop, bool rising, const float* const* cv, int k) {
    const int i = G.first;
    const float pot = clamp01(param(kPot + i));
    if (!loop && !G.gated) {  // sistema caótico de Chen (a=42, b de 1 a 6 pelo pot, c=28)
      float h = 0.7f * lfoFreq(rangeOf(i), param(kSlider + i) + cv[i][k]) / sampleRate_;  // [ASSUMIDO] escala de tempo
      const int sub = std::min(16, 1 + static_cast<int>(h / 0.002f));
      h /= static_cast<float>(sub);
      const float a = 42.f, b = 1.f + 5.f * pot, c = 28.f;
      for (int s = 0; s < sub; ++s) {
        const float dx = a * (G.cy - G.cx), dy = (c - a) * G.cx - G.cx * G.cz + c * G.cy, dz = G.cx * G.cy - b * G.cz;
        G.cx += h * dx; G.cy += h * dy; G.cz += h * dz;
      }
      if (!std::isfinite(G.cx) || std::fabs(G.cx) > 500.f) { G.cx = -3.f; G.cy = 2.f; G.cz = 20.f; }
      G.out = polarOut(clamp01(0.5f + G.cx / 50.f), bipolar(i));  // [ASSUMIDO] escala de saída
    } else if (!loop) {  // registrador de deslocamento probabilístico (clock = gate)
      const int steps = 1 + static_cast<int>(pot * 15.f + 0.5f);
      if (rising) stepRegister(G, steps, clamp01(param(kSlider + i) + cv[i][k]));
      G.out = regValue(G, i, cv, k);
    } else {  // LFO aleatório (livre ou sincronizado)
      advancePhase(G, i, rising, cv, k, true);
      if (G.wrapped) {
        G.lfoPrev = G.lfoNext;
        G.lfoNext = pot <= 0.5f ? rand01() : clamp01(G.lfoPrev + (rand01() - 0.5f) * 2.f * (1.f - (pot - 0.5f) * 1.8f));
      }
      const float sm = 0.5f - 0.5f * std::cos(kPi * G.lfoPhase);
      const float smooth = pot < 0.5f ? pot * 2.f : 1.f;  // 0 = em degraus, 1 = interpolado
      G.out = polarOut(G.lfoPrev + (G.lfoNext - G.lfoPrev) * sm * smooth, bipolar(i));
    }
  }

  // ---- Sequenciador estendido (firmware oficial 1.2) ---------------------------------
  // Um HOLD ou RAMP no início de uma sequência de STEPs. O primeiro segmento não é um passo: o pot dele escolhe
  // o modo de reprodução, o gate dele é o clock, o slider + CV viram endereço (modo endereçável) ou reset (nos
  // demais), e a CV da sequência sai pela saída dele. Primeiro HOLD: sem quantização, 0 a 8 V. Primeiro RAMP:
  // 0 a 1 V, passos quantizados em semitons. Um loop restringe a reprodução à parte em loop.
  // [ASSUMIDO] detecção (primeiro HOLD/RAMP + ao menos 2 STEPs), zonas iguais do pot para os 7 modos, limiar de
  // reset de 4 V e que o primeiro clock toca o passo 1 (o manual não diz).
  bool isExtSeq(const Group& G, const int* types) const {
    if (types[G.first] != kRamp && types[G.first] != kHold) return false;
    for (int j = G.first + 1; j <= G.last; ++j) if (types[j] != kStep) return false;
    return true;
  }
  static int pendulumPos(int j, int m) {  // posições 1..m e de volta, sem repetir as pontas
    const int period = std::max(1, 2 * (m - 1)), r = j % period;
    return r < m ? 1 + r : 1 + (period - r);
  }
  void advanceSeq(Group& G, int mode, int n) {
    if (!G.seqStarted) {  // primeiro clock: começa na primeira posição do modo
      G.seqStarted = true; G.seqCounter = 0; G.seqDir = 1;
      G.seqPos = mode == 1 ? n - 1 : (mode >= 4 ? std::min(n - 1, static_cast<int>(rand01() * n)) : 0);
      if (mode == 1) G.seqDir = -1;
      return;
    }
    switch (mode) {
      case 0: G.seqPos = (G.seqPos + 1) % n; break;
      case 1: G.seqPos = (G.seqPos + n - 1) % n; break;
      case 2:
        if (n > 1) { if (G.seqPos + G.seqDir >= n || G.seqPos + G.seqDir < 0) G.seqDir = -G.seqDir; G.seqPos += G.seqDir; }
        break;
      case 3:  // alternado: 1, 2, 1, 3, 1, 4, 1, 3, 1, 2...
        ++G.seqCounter;
        G.seqPos = (G.seqCounter % 2 == 0 || n < 2) ? 0 : pendulumPos((G.seqCounter - 1) / 2, n - 1);
        break;
      case 4: G.seqPos = std::min(n - 1, static_cast<int>(rand01() * n)); break;
      default:  // pseudoaleatório sem repetir o passo anterior
        if (n > 1) { int p = std::min(n - 2, static_cast<int>(rand01() * (n - 1))); G.seqPos = p >= G.seqPos ? p + 1 : p; }
        break;
    }
  }
  void runExtendedSeq(Group& G, const int* types, bool loop, int ls, int le, bool rising,
                      const float* const* cv, int k) {
    const int f = G.first;
    int rs = f + 1, re = G.last;
    if (loop) { rs = std::max(rs, ls); re = std::min(re, le); if (re < rs) { rs = f + 1; re = G.last; } }
    const int n = re - rs + 1;
    const float x = param(kSlider + f) + cv[f][k];
    const int mode = std::min(6, std::max(0, static_cast<int>(clamp01(param(kPot + f)) * 7.f)));
    if (mode == 6) {  // endereçável: o clock é ignorado
      G.seqPos = std::min(n - 1, static_cast<int>(clamp01(x) * static_cast<float>(n)));
      G.seqStarted = true;
    } else {
      const bool rh = x > 0.5f;
      if (rh && !G.resetHigh) {  // borda de subida: volta ao primeiro passo e inibe o clock por 5 ms
        G.seqPos = 0; G.seqStarted = true; G.seqDir = 1; G.seqCounter = 0;
        G.inhibit = static_cast<int>(0.005f * sampleRate_);
      }
      G.resetHigh = rh;
      if (G.inhibit > 0) --G.inhibit;
      if (rising && G.inhibit == 0 && !G.resetHigh) advanceSeq(G, mode, n);
    }
    G.seqPos = std::min(G.seqPos, n - 1);
    const int j = rs + G.seqPos;
    G.active = j;
    if (types[f] == kHold) {
      G.out = segLevel(j, cv, k);  // amostrado continuamente
    } else {
      const float volts = std::min(1.f, std::max(0.f, param(kSlider + j) + cv[j][k] * 8.f));
      G.out = std::round(volts * 12.f) / 12.f / 8.f;  // 0..1 V em semitons
    }
  }

  // ---- Grupo com 2 ou mais segmentos ---------------------------------------------
  bool trapped(const int* types, int i) const { return types[i] == kStep || types[i] == kRandom; }

  float valueOf(const Group& G, int j, const int* types, const float* const* cv, int k) const {
    return types[j] == kRandom ? regValue(G, j, cv, k) : segLevel(j, cv, k);
  }

  void runMulti(Group& G, const int* types, bool loop, int ls, int le, bool gateHigh, bool rising,
                const float* const* cv, int k) {
    if (rising) {
      if (G.active >= 0 && types[G.active] == kRamp && noRetrig(G.active)) {
        // re-trigger desligado: gate ignorado enquanto esta rampa está ativa
      } else if (G.active >= 0 && trapped(types, G.active)) {
        goNext(G, types, loop, ls, le, gateHigh, cv, k);
      } else {
        startSeg(G, G.first);
      }
    }
    if (G.active < 0) return;
    const int i = G.active;
    const float pot = clamp01(param(kPot + i));
    switch (types[i]) {
      case kRamp: {
        const float end = rampEnd(G, i, types, cv, k);
        // firmware 1.2: a rampa acompanha o nível do segmento anterior (pseudo-VCA / crossfade)
        const float start = (i > G.first && (types[i - 1] == kStep || types[i - 1] == kHold)) ? segLevel(i - 1, cv, k) : G.startLevel;
        G.phase += 1.f / (rampTime(rangeOf(i), param(kSlider + i) + cv[i][k]) * sampleRate_);
        if (G.phase >= 1.f) { G.phase = 1.f; G.out = end; goNext(G, types, loop, ls, le, gateHigh, cv, k); }
        else G.out = start + (end - start) * warp(G.phase, pot);
        break;
      }
      case kHold:
        G.out = segLevel(i, cv, k);
        G.holdDur = sliderToTime(pot);
        G.timer += 1.f / sampleRate_;
        if (G.timer >= G.holdDur) goNext(G, types, loop, ls, le, gateHigh, cv, k);
        break;
      case kStep:
        G.out += (segLevel(i, cv, k) - G.out) * glideCoef(pot);
        break;
      default:  // kRandom em grupo: segura o valor do registrador (simplificado, sem loop próprio)
        G.out = regValue(G, i, cv, k);
        break;
    }
  }

  void startSeg(Group& G, int i) { G.active = i; G.phase = 0.f; G.timer = 0.f; G.startLevel = G.out; }

  void goNext(Group& G, const int* types, bool loop, int ls, int le, bool gateHigh,
              const float* const* cv, int k) {
    const int cur = G.active;
    if (types[cur] == kRandom) stepRegister(G, 1 + static_cast<int>(clamp01(param(kPot + cur)) * 15.f + 0.5f), clamp01(param(kSlider + cur) + cv[cur][k]));
    int nxt = cur + 1;
    if (loop && cur == le) {
      bool hasTrap = false;
      for (int j = ls; j <= le; ++j) hasTrap = hasTrap || trapped(types, j);
      if (gateHigh || le == G.last || hasTrap) nxt = ls;
    }
    if (nxt > G.last) G.active = -1; else startSeg(G, nxt);
  }

  float rampEnd(const Group& G, int i, const int* types, const float* const* cv, int k) const {
    if (i + 1 <= G.last && types[i + 1] != kRamp) return valueOf(G, i + 1, types, cv, k);
    bool lastRamp = true, firstRamp = true;
    for (int j = i + 1; j <= G.last; ++j) lastRamp = lastRamp && types[j] != kRamp;
    for (int j = G.first; j < i; ++j) firstRamp = firstRamp && types[j] != kRamp;
    if (lastRamp) return 0.f;
    if (firstRamp) return 1.f;
    return clamp01(param(kPot + i));
  }

  // ---- Modos DAHDSR (2: independentes, 3: idênticos) -------------------------------
  // Fases: 1 delay, 2 attack, 3 hold (sempre 8 V), 4 decay, 5 sustain, 6 release.
  float envParam(bool identical, int e, int k) const {
    if (identical) return k < 6 ? param(kSlider + k) : param(kPot + 1 + 2 * (k - 6));  // pots 2, 4 e 6 = formas
    return param(kEnv + e * 9 + k);
  }
  void runDahdsr(int n, bool identical) {
    const float dt = 1.f / sampleRate_;
    for (int e = 0; e < kN; ++e) {
      const float* g = in(e);
      float* o = out(e);
      Env& E = env_[e];
      for (int s = 0; s < n; ++s) {
        auto val = [&](int k) { return clamp01(envParam(identical, e, k) + (identical ? in(kN + k)[s] : 0.f)); };
        const bool gate = g[s] > 0.5f;
        if (gate && !E.prevGate) { E.phase = 1; E.t = 0.f; }
        if (!gate && E.prevGate && E.phase != 0 && E.phase != 6) { E.phase = 6; E.t = 0.f; E.startLevel = E.level; }
        E.prevGate = gate;
        switch (E.phase) {
          case 1: { const float d = dahdsrTime(val(0)); if (d <= 0.f || E.t >= d) { E.phase = 2; E.t = 0.f; E.startLevel = E.level; } else E.t += dt; break; }
          case 2: {
            const float d = dahdsrTime(val(1));
            if (d <= 0.f) { E.level = 1.f; E.phase = 3; E.t = 0.f; break; }
            E.t += dt; const float p = std::min(1.f, E.t / d);
            E.level = E.startLevel + (1.f - E.startLevel) * warp(p, clamp01(envParam(identical, e, 6)));
            if (p >= 1.f) { E.phase = 3; E.t = 0.f; }
            break;
          }
          case 3: { const float d = dahdsrTime(val(2)); E.level = 1.f; if (d <= 0.f || E.t >= d) { E.phase = 4; E.t = 0.f; } else E.t += dt; break; }
          case 4: {
            const float d = dahdsrTime(val(3)), sus = val(4);
            if (d <= 0.f) { E.level = sus; E.phase = 5; break; }
            E.t += dt; const float p = std::min(1.f, E.t / d);
            E.level = 1.f + (sus - 1.f) * warp(p, clamp01(envParam(identical, e, 7)));
            if (p >= 1.f) E.phase = 5;
            break;
          }
          case 5: E.level = val(4); break;
          case 6: {
            const float d = dahdsrTime(val(5));
            if (d <= 0.f) { E.level = 0.f; E.phase = 0; break; }
            E.t += dt; const float p = std::min(1.f, E.t / d);
            E.level = E.startLevel * (1.f - warp(p, clamp01(envParam(identical, e, 8))));
            if (p >= 1.f) { E.level = 0.f; E.phase = 0; }
            break;
          }
          default: break;
        }
        o[s] = E.level;
      }
    }
  }


  // ---- Ouroboros (modos 4 e 5) -------------------------------------------------------
  void runHarmonic(int n, bool alt) {
    const int rootRange = std::min(2, std::max(0, static_cast<int>(param(kHarmRootRange) + 0.5f)));
    const bool lfoRange = rootRange != 2;
    const float dtS = 1.f / sampleRate_;
    const float strumDecay = std::exp(-dtS / 0.12f);  // [ASSUMIDO] strum: decaimento rápido de ~120 ms
    const float* gate[kN]; const float* cv[kN]; float* o[kN];
    for (int i = 0; i < kN; ++i) { gate[i] = in(i); cv[i] = in(kN + i); o[i] = out(i); }
    for (int s = 0; s < n; ++s) {
      const float fine = (param(kPot) - 0.5f) * (2.f / 12.f);  // [ASSUMIDO] afinação fina de ±1 semitom
      const float rootF = harmRootFreq(rootRange, param(kSlider), fine + cv[0][s]);
      float mix = 0.f, total = 0.f;
      for (int i = 0; i < kN; ++i) {
        HarmState& H = harm_[i];
        const bool gh = gate[i][s] > 0.5f, rising = gh && !H.prevGate;
        H.prevGate = gh;
        float f = rootF, vol = 1.f;
        if (i > 0) {
          const int rr = std::min(2, std::max(0, static_cast<int>(param(kHarmRatioRange + i) + 0.5f)));
          const float x = alt ? clamp01(param(kSlider + i) + cv[i][s]) : clamp01(param(kPot + i));
          vol = alt ? clamp01(param(kPot + i)) : clamp01(param(kSlider + i) + cv[i][s]);
          f = rootF * harmRatio(rr, x);
        }
        if (rising) { H.strum = 1.f; if (lfoRange) H.phase = 0.f; }  // gate = strum (e, em LFO, reseta a fase)
        const float dt = f * dtS;
        const int w = std::min(5, std::max(0, static_cast<int>(param(kWave + i) + 0.5f)));
        const float v = (!lfoRange && f > 0.45f * sampleRate_) ? 0.f : harmWave(w, H.phase, dt);
        H.phase += dt;
        H.phase -= std::floor(H.phase);
        H.strum *= strumDecay;
        harmVol_[i] = vol;
        float amp = i == 0 ? 1.f : vol + (1.f - vol) * H.strum;
        // Em faixa de LFO, um gate ligado (ou cabo fantasma) tira o parcial do mix: ele só aparece nos strums.
        if (lfoRange && (inputConnected(i) || param(kDummyGate + i) > 0.5f)) amp = H.strum;
        mix += v * amp;
        total += amp;
        if (i > 0) o[i][s] = lfoRange ? v * vol : v;  // em faixa de áudio o volume só vale para o mix
      }
      o[0][s] = mix / std::max(1.f, total);
    }
  }

  // ---- LEDs do painel ------------------------------------------------------------------
  // Um LED por segmento (o manual fala nos "seis LEDs"). Cores (README do qiemem): verde = ramp,
  // laranja = step, vermelho = hold, aleatório = oscila suave entre as três. O brilho acompanha o
  // sinal de saída do segmento. Loop = pisca; loop de vários segmentos = "ping pong" entre o primeiro e o
  // último; bipolar / sem re-trigger = piscada vermelha fraca 1x por segundo. Modo daltônico (firmware 1.1):
  // ramp com brilho moderado e leve modulação, step brilhante, hold fraco. [ASSUMIDO] taxas de piscar,
  // piso de brilho e curva de brilho (o manual só mostra isso em imagens).
  // LEDs 6..11: ao lado de cada saída (na placa escura numerada da foto do painel). [ASSUMIDO] mostram o nível
  // da saída (pico do bloco) na cor do tipo; as fontes que li não descrevem a função desses LEDs.
  void setOutLed(int i, const float* c, int n) {
    float pk = 0.f;
    for (int k = 0; k < n; ++k) pk = std::max(pk, std::fabs(out(i)[k]));
    pk = std::pow(clamp01(pk), 0.7f);
    setLed(kN + i, c[0] * pk, c[1] * pk, c[2] * pk);
  }
  static void typeColor(int t, float* c) {
    static const float col[3][3] = {{0.f, 1.f, 0.f}, {1.f, 0.45f, 0.f}, {1.f, 0.f, 0.f}};
    for (int k = 0; k < 3; ++k) c[k] = col[std::min(2, t)][k];
  }
  void segmentLeds(int n) {
    const bool cb = param(kColorBlind) > 0.5f;
    for (int i = 0; i < kN; ++i) {
      const int t = ledType_[i];
      float c[3];
      if (t == kRandom) {
        const float u = 0.5f + 0.5f * std::sin(2.f * kPi * 0.4f * ledT_);
        float a[3], b[3];
        typeColor(u < 0.5f ? 0 : 1, a); typeColor(u < 0.5f ? 1 : 2, b);
        const float m = u < 0.5f ? u * 2.f : (u - 0.5f) * 2.f;
        for (int k = 0; k < 3; ++k) c[k] = a[k] + (b[k] - a[k]) * m;
      } else {
        typeColor(t, c);
      }
      const float sig = clamp01(std::fabs(out(i)[n - 1]));
      float bright = 0.2f + 0.8f * sig;
      if (cb) {
        static const float tb[4] = {0.55f, 1.f, 0.25f, 0.7f};
        bright = tb[t] * (0.7f + 0.3f * sig);
        if (t == kRamp) bright *= 1.f + 0.1f * std::sin(2.f * kPi * ledT_);
      }
      const int role = ledLoop_[i];
      if (role) {
        float rate = 1.5f;
        if (role == 1 && t == kRamp) { const int r = rangeOf(i); rate = r == 0 ? 0.5f : (r == 2 ? 4.f : 1.5f); }
        const float sn = std::sin(2.f * kPi * (ledT_ * rate + (role == 3 ? 0.5f : 0.f)));
        bright *= t == kRamp ? 0.5f + 0.5f * sn : (sn > 0.f ? 1.f : 0.f);
      }
      float r = c[0] * bright, g = c[1] * bright, b = c[2] * bright;
      if (ledFlag_[i] && std::fmod(ledT_, 1.f) < 0.1f) r = std::max(r, 0.3f);  // piscada vermelha fraca
      setLed(i, r, g, b);
      setOutLed(i, c, n);
    }
  }
  void dahdsrLeds(bool identical, int n) {
    const int sel = std::min(5, std::max(0, static_cast<int>(param(kEnvSelected) + 0.5f)));
    for (int e = 0; e < kN; ++e) {
      float r = 0.f, g = 0.f;
      switch (env_[e].phase) {
        case 1: case 2: case 3: case 4: g = 1.f; break;   // delay, attack, hold, decay
        case 5: r = 1.f; g = 0.45f; break;                // sustain
        case 6: r = 1.f; break;                           // release
        default: if (!identical && e == sel) { r = 1.f; g = 0.85f; }  // selecionado em repouso: amarelo
      }
      setLed(e, r, g, 0.f);
      const float c[3] = {r, g, 0.f};
      setOutLed(e, c, n);
    }
  }
  void harmonicLeds(int n) {
    for (int i = 0; i < kN; ++i) {
      const int w = std::min(5, std::max(0, static_cast<int>(param(kWave + i) + 0.5f)));
      float c[3];
      typeColor(w % 3, c);
      float bright = i == 0 ? 1.f : 0.4f + 0.6f * harmVol_[i];
      if (w >= 3) bright *= std::sin(2.f * kPi * 2.f * ledT_) > 0.f ? 1.f : 0.2f;  // formas "piscando"
      setLed(i, c[0] * bright, c[1] * bright, c[2] * bright);
      setOutLed(i, c, n);
    }
  }

  Group groups_[kN];
  int ngroups_ = 0, mask_ = -1, lastMode_ = 0;
  bool advanced_ = false;
  uint32_t rng_ = 2463534242u;
  std::vector<float> cvDelay_[kN];
  Env env_[kN];
  HarmState harm_[kN];
  float harmVol_[kN] = {};
  float ledT_ = 0.f;
  int ledType_[kN] = {}, ledLoop_[kN] = {};
  bool ledFlag_[kN] = {};
};

}  // namespace mod
