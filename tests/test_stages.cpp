// Testes do Stages: reproduzem exemplos do manual (ADSR, AD, sequência de steps,
// S&H, gerador de pulso, LFO, atraso de CV, decaimento).
// Compilar: g++ -std=c++17 -O2 -Isrc tests/test_stages.cpp -o test_stages && ./test_stages

#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "engine.h"
#include "stages.h"

using mod::Engine;
using mod::Module;
using mod::Stages;

namespace {

constexpr float kSr = 48000.f;

// Fonte com saídas definidas por funções do tempo (em segundos).
class Src : public Module {
 public:
  explicit Src(int n) : Module(0, n), fn(n) {}
  void process(int frames) override {
    for (int p = 0; p < numOut(); ++p)
      for (int k = 0; k < frames; ++k) out(p)[k] = fn[p] ? fn[p]((pos_ + k) / kSr) : 0.f;
    pos_ += frames;
  }
  std::vector<std::function<float(double)>> fn;

 private:
  long pos_ = 0;
};

// Grava as 6 saídas do Stages.
class Probe : public Module {
 public:
  Probe() : Module(6, 0), rec(6) {}
  void process(int frames) override {
    for (int p = 0; p < 6; ++p) rec[p].insert(rec[p].end(), in(p), in(p) + frames);
    if (watch)  // amostra os LEDs uma vez por bloco (96 frames = 2 ms)
      for (int i = 0; i < 12; ++i) { std::array<float, 3> c; watch->getLed(i, c.data()); led[i].push_back(c); }
  }
  std::vector<std::vector<float>> rec;
  const Stages* watch = nullptr;
  std::array<std::vector<std::array<float, 3>>, 12> led;
};

struct Rig {
  Engine eng{kSr, 96};
  Stages* st;
  Src* src;
  Probe* pr;
  Rig() {
    st = eng.add<Stages>();
    src = eng.add<Src>(12);
    pr = eng.add<Probe>();
    pr->watch = st;
  }
  void gate(int seg, std::function<float(double)> f) {
    src->fn[seg] = f;
    eng.connect(src, seg, st, seg);
  }
  void cv(int seg, std::function<float(double)> f) {
    src->fn[6 + seg] = f;
    eng.connect(src, 6 + seg, st, 6 + seg);
  }
  void seg(int i, int type, float slider, float pot) {
    st->setParam(Stages::kType + i, static_cast<float>(type));
    st->setParam(Stages::kSlider + i, slider);
    st->setParam(Stages::kPot + i, pot);
  }
  void loop(int a, int b) {
    st->setParam(Stages::kLoopStart + a, 1.f);
    st->setParam(Stages::kLoopEnd + b, 1.f);
  }
  void dummy(int i) { st->setParam(Stages::kDummyGate + i, 1.f); }
  bool run(double seconds) {
    for (int p = 0; p < 6; ++p) eng.connect(st, p, pr, p);
    if (!eng.compile(nullptr)) return false;
    std::vector<float> l(4800), r(4800);
    for (int done = 0; done < static_cast<int>(seconds * kSr); done += 4800)
      eng.render(l.data(), r.data(), 4800);
    return true;
  }
  std::array<float, 3> ledAt(int i, double t) const { return pr->led[i][static_cast<size_t>(t * kSr / 96.0)]; }
  float lumAt(int i, double t) const { auto c = ledAt(i, t); return std::max(c[0], std::max(c[1], c[2])); }
  float at(int out, double t) const { return pr->rec[out][static_cast<size_t>(t * kSr)]; }
  float minOf(int out) const {
    float m = 9.f;
    for (float v : pr->rec[out]) m = std::min(m, v);
    return m;
  }
  float maxOf(int out) const {
    float m = -9.f;
    for (float v : pr->rec[out]) m = std::max(m, v);
    return m;
  }
};

int failures = 0;
void check(const char* name, bool ok, const char* detail = "") {
  std::printf("[%s] %s %s\n", ok ? "OK " : "FALHA", name, detail);
  if (!ok) ++failures;
}
bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
float pulse(double t, double t0, double t1) { return (t >= t0 && t < t1) ? 1.f : 0.f; }
float T(float s) { return Stages::timeToSlider(s); }


int crossings(const Rig& g, int out, double t0, double t1) {
  const auto& v = g.pr->rec[out];
  int c = 0;
  for (size_t i = static_cast<size_t>(t0 * kSr) + 1; i < static_cast<size_t>(t1 * kSr); ++i)
    if (v[i - 1] < 0.f && v[i] >= 0.f) ++c;
  return c;
}
double corr(const Rig& g, int a, int b, double t0, double t1) {
  double sum = 0;
  size_t i0 = static_cast<size_t>(t0 * kSr), i1 = static_cast<size_t>(t1 * kSr);
  for (size_t i = i0; i < i1; ++i) sum += g.pr->rec[a][i] * g.pr->rec[b][i];
  return sum / static_cast<double>(i1 - i0);
}
void harmSetup(Rig& g, int mode, float rootSlider) {  // parciais mudos, raiz na frequência dada
  g.st->setParam(Stages::kMode, static_cast<float>(mode));
  g.st->setParam(Stages::kSlider + 0, rootSlider);
  for (int i = 1; i < 6; ++i) { g.st->setParam(Stages::kSlider + i, 0.f); g.st->setParam(Stages::kPot + i, 0.f); }
}
float rootX(float hz) { return std::log2(hz / 16.3516f) / 8.f; }


// Roda uma sequência estendida: [RAMP|HOLD][STEP x4], clock a cada 0,1 s (borda em k*0,1+0,05).
std::vector<float> extSeq(float firstType, float modePot, const float* sliders, int clocks, bool loop23 = false) {
  Rig g;
  g.gate(0, [](double t) { return std::fmod(t, 0.1) >= 0.05 && std::fmod(t, 0.1) < 0.07 ? 1.f : 0.f; });
  g.dummy(5);
  g.seg(0, static_cast<int>(firstType), 0.f, modePot);
  for (int i = 0; i < 4; ++i) g.seg(1 + i, Stages::kStep, sliders[i], 0.f);
  if (loop23) g.loop(2, 3);
  g.st->seed(4242u);
  g.run(clocks * 0.1 + 0.2);
  std::vector<float> v;
  for (int k = 0; k < clocks; ++k) v.push_back(g.at(0, k * 0.1 + 0.09));
  return v;
}
int stepIndex(float v, const float* levels) {  // qual dos 4 níveis esperados
  for (int i = 0; i < 4; ++i) if (near(v, levels[i], 0.0006f)) return i;
  return -1;
}

}  // namespace

int main() {
  const float R = Stages::kRamp, S = Stages::kStep, H = Stages::kHold, X = Stages::kRandom;
  auto ds = [](float sec) { return Stages::dahdsrSlider(sec); };

  {  // ADSR: [ramp][ramp][hold com loop][ramp]
    Rig g;
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.6); });
    g.dummy(4);  // separa os segmentos 5 e 6 do grupo
    g.seg(0, R, T(0.010f), 0.5f);
    g.seg(1, R, T(0.050f), 0.5f);
    g.seg(2, H, 0.5f, 0.5f);
    g.seg(3, R, T(0.100f), 0.5f);
    g.loop(2, 2);
    check("ADSR compila", g.run(1.2));
    check("ADSR pico = 8 V", near(g.maxOf(0), 1.f, 0.02f));
    check("ADSR sustenta no nível do HOLD", near(g.at(0, 0.4), 0.5f, 0.02f));
    check("ADSR relaxa a 0 V", near(g.at(0, 1.0), 0.f, 0.01f));
    check("ADSR atividade do HOLD durante o sustain", g.at(2, 0.4) > 0.f && g.at(1, 0.4) == 0.f);
    check("ADSR nunca passa de 8 V", g.maxOf(0) < 1.0001f);
  }

  {  // AD: [ramp][ramp]
    Rig g;
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.11); });
    g.dummy(2);
    g.seg(0, R, T(0.020f), 0.5f);
    g.seg(1, R, T(0.200f), 0.5f);
    check("AD compila", g.run(0.6));
    check("AD pico = 8 V", near(g.maxOf(0), 1.f, 0.02f));
    check("AD decai linear (meio)", near(g.at(0, 0.1 + 0.020 + 0.100), 0.5f, 0.05f));
    check("AD termina em 0 V", near(g.at(0, 0.55), 0.f, 0.01f));
  }

  {  // Sequência de 3 steps em loop; cada trigger avança
    Rig g;
    g.gate(0, [](double t) {
      return pulse(t, 0.1, 0.15) + pulse(t, 0.3, 0.35) + pulse(t, 0.5, 0.55) + pulse(t, 0.7, 0.75);
    });
    g.dummy(3);
    g.seg(0, S, 0.25f, 0.f);
    g.seg(1, S, 0.50f, 0.f);
    g.seg(2, S, 0.75f, 0.f);
    g.loop(0, 2);
    check("Sequência compila", g.run(1.0));
    check("Passo 1", near(g.at(0, 0.12), 0.25f, 0.01f));
    check("Passo 2", near(g.at(0, 0.32), 0.50f, 0.01f));
    check("Passo 3", near(g.at(0, 0.52), 0.75f, 0.01f));
    check("Volta ao passo 1 (loop)", near(g.at(0, 0.72), 0.25f, 0.01f));
    check("Segura entre triggers", near(g.at(0, 0.25), 0.25f, 0.01f));
  }

  {  // Step isolado com gate = sample & hold
    Rig g;
    g.gate(5, [](double t) { return pulse(t, 0.25, 0.27) + pulse(t, 0.75, 0.77); });
    g.cv(5, [](double t) { return static_cast<float>(t); });  // rampa 0..1 em 1 s
    g.seg(5, S, 0.f, 0.f);
    check("S&H compila", g.run(1.0));
    check("S&H amostra no 1º trigger", near(g.at(5, 0.5), 0.25f, 0.015f));
    check("S&H amostra no 2º trigger", near(g.at(5, 0.9), 0.75f, 0.015f));
  }

  {  // Hold isolado com gate = gerador de pulso
    Rig g;
    g.gate(5, [](double t) { return pulse(t, 0.1, 0.11); });
    g.seg(5, H, 0.6f, T(0.1f));
    check("Pulso compila", g.run(0.5));
    check("Pulso antes do trigger = 0", near(g.at(5, 0.05), 0.f, 0.001f));
    check("Pulso durante = nível", near(g.at(5, 0.15), 0.6f, 0.001f));
    check("Pulso depois = 0", near(g.at(5, 0.25), 0.f, 0.001f));
  }

  {  // Ramp isolado em loop, sem gate = LFO livre de 10 Hz
    Rig g;
    g.seg(0, R, Stages::lfoSliderForFreq(1, 10.f), 0.5f);
    g.loop(0, 0);
    check("LFO compila", g.run(1.0));
    int cross = 0;
    const auto& v = g.pr->rec[0];
    for (size_t i = 1; i < v.size(); ++i)
      if (v[i - 1] < 0.5f && v[i] >= 0.5f) ++cross;
    char d[48];
    std::snprintf(d, sizeof(d), "(%d ciclos em 1 s)", cross);
    check("LFO ~10 Hz", cross >= 9 && cross <= 11, d);
    check("LFO fica entre 0 e 8 V", g.maxOf(0) <= 1.0001f && g.maxOf(0) > 0.95f);
    check("Rampa livre sem loop fica ociosa", near(g.maxOf(1), 0.f, 0.0001f));
  }

  {  // Hold isolado sem gate = atraso de CV
    Rig g;
    g.cv(0, [](double t) { return t < 0.1 ? 0.f : 0.8f; });
    g.seg(0, H, 0.f, T(0.2f));
    check("Atraso de CV compila", g.run(0.6));
    check("Atraso: ainda 0 antes do atraso", near(g.at(0, 0.25), 0.f, 0.001f));
    check("Atraso: 0,8 depois do atraso", near(g.at(0, 0.35), 0.8f, 0.001f));
  }

  {  // Ramp isolado com gate = envelope de decaimento
    Rig g;
    g.gate(5, [](double t) { return pulse(t, 0.1, 0.11); });
    g.seg(5, R, T(0.2f), 0.5f);
    check("Decaimento compila", g.run(0.6));
    check("Decaimento começa em 8 V", g.at(5, 0.1005) > 0.95f);
    check("Decaimento no meio", near(g.at(5, 0.2), 0.5f, 0.05f));
    check("Decaimento termina em 0", near(g.at(5, 0.45), 0.f, 0.001f));
  }

  {  // LFO sincronizado a um clock de 10 Hz (razão 1)
    Rig g;
    g.gate(5, [](double t) { return std::fmod(t, 0.1) < 0.01 ? 1.f : 0.f; });
    g.seg(5, R, 0.5f, 0.5f);
    g.loop(5, 5);
    check("LFO sync compila", g.run(1.6));
    int cross = 0;
    const auto& v = g.pr->rec[5];
    for (size_t i = static_cast<size_t>(0.5 * kSr); i < static_cast<size_t>(1.5 * kSr); ++i)
      if (v[i - 1] < 0.5f && v[i] >= 0.5f) ++cross;
    char d[48];
    std::snprintf(d, sizeof(d), "(%d ciclos em 1 s)", cross);
    check("LFO sync segue o clock (~10 Hz)", cross >= 9 && cross <= 11, d);
  }

  {  // Gate só no segmento 5 (índice 4): 0..3 livres, 4..5 em grupo
    Rig g;
    g.gate(4, [](double t) { return pulse(t, 0.1, 0.2); });
    g.seg(4, R, T(0.01f), 0.5f);
    g.seg(5, R, T(0.05f), 0.5f);
    check("Grupos independentes compilam", g.run(0.5));
    bool finite = true;
    for (int p = 0; p < 6; ++p)
      for (float x : g.pr->rec[p]) finite = finite && std::isfinite(x);
    check("Saídas finitas", finite);
    check("Grupo [5,6] gera envelope", near(g.maxOf(4), 1.f, 0.02f));
  }


  // ======================= firmware do qiemem =======================
  {  // Modo 3: seis DAHDSR idênticos (delay, attack, hold, decay, sustain, release)
    Rig g;
    g.st->setParam(Stages::kMode, 3);
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.8); });
    const float v[6] = {ds(0.1f), ds(0.05f), ds(0.1f), ds(0.1f), 0.5f, ds(0.2f)};
    for (int k = 0; k < 6; ++k) g.st->setParam(Stages::kSlider + k, v[k]);
    check("DAHDSR compila", g.run(1.3));
    check("DAHDSR: delay mantém 0", near(g.at(0, 0.15), 0.f, 0.01f));
    check("DAHDSR: hold fica em 8 V", near(g.at(0, 0.3), 1.f, 0.01f));
    check("DAHDSR: decay linear (meio)", near(g.at(0, 0.4), 0.75f, 0.03f));
    check("DAHDSR: sustain", near(g.at(0, 0.6), 0.5f, 0.01f));
    check("DAHDSR: release (meio)", near(g.at(0, 0.9), 0.25f, 0.04f));
    check("DAHDSR: termina em 0", near(g.at(0, 1.2), 0.f, 0.01f));
    check("DAHDSR: outros envelopes quietos", near(g.maxOf(1), 0.f, 0.0001f));
  }
  {  // Modo 2: seis DAHDSR independentes (cada um com seus ajustes)
    Rig g;
    g.st->setParam(Stages::kMode, 2);
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.6); });
    g.gate(1, [](double t) { return pulse(t, 0.1, 0.6); });
    const float atk[2] = {0.05f, 0.2f};
    for (int e = 0; e < 2; ++e) {
      g.st->setParam(Stages::kEnv + e * 9 + 1, ds(atk[e]));
      g.st->setParam(Stages::kEnv + e * 9 + 4, 1.f);
      g.st->setParam(Stages::kEnv + e * 9 + 5, ds(0.1f));
    }
    check("DAHDSR independentes compilam", g.run(0.8));
    check("Env 1: ataque curto (meio)", near(g.at(0, 0.125), 0.5f, 0.06f));
    check("Env 2: ataque longo (1/8)", near(g.at(1, 0.125), 0.125f, 0.04f));
    check("Env 1 chega a 8 V", near(g.at(0, 0.2), 1.f, 0.01f));
  }
  {  // LFO bipolar: -5 V a +5 V
    Rig g;
    g.seg(0, R, Stages::lfoSliderForFreq(1, 10.f), 0.5f);
    g.loop(0, 0);
    g.st->setParam(Stages::kBipolar + 0, 1.f);
    check("LFO bipolar compila", g.run(0.5));
    check("LFO bipolar: -5 V a +5 V", near(g.minOf(0), -0.625f, 0.02f) && near(g.maxOf(0), 0.625f, 0.02f));
  }
  {  // Quantização de step: cromática, maior e pentatônica (slider vale 0..2 V)
    Rig g;
    g.seg(0, S, 7.2f / 24.f, 0.f); g.st->setParam(Stages::kQuant + 0, Stages::kChromatic);
    g.seg(1, S, 6.2f / 24.f, 0.f); g.st->setParam(Stages::kQuant + 1, Stages::kMajor);
    g.seg(2, S, 5.4f / 24.f, 0.f); g.st->setParam(Stages::kQuant + 2, Stages::kPentatonic);
    check("Quantização compila", g.run(0.2));
    check("Cromática: 7,2 -> 7 semitons", near(g.at(0, 0.15), 7.f / 96.f, 0.001f));
    check("Maior: 6,2 -> 7 semitons", near(g.at(1, 0.15), 7.f / 96.f, 0.001f));
    check("Pentatônica: 5,4 -> 4 semitons", near(g.at(2, 0.15), 4.f / 96.f, 0.001f));
  }
  {  // Re-trigger desligado: gate durante a rampa é ignorado
    float vals[2];
    for (int variant = 0; variant < 2; ++variant) {
      Rig g;
      g.gate(0, [](double t) { return pulse(t, 0.1, 0.11) + pulse(t, 0.2, 0.21); });
      g.dummy(2);
      g.seg(0, R, T(0.2f), 0.5f); g.seg(1, R, T(1.0f), 0.5f);
      if (variant == 1) g.st->setParam(Stages::kNoRetrig + 0, 1.f);
      g.run(0.4);
      vals[variant] = g.at(0, 0.25);
    }
    check("Re-trigger ligado reinicia o ataque", near(vals[0], 0.625f, 0.02f));
    check("Re-trigger desligado ignora o gate", near(vals[1], 0.75f, 0.02f));
  }
  {  // Faixa de tempo rápida da rampa (1 ms a 2,2 s)
    Rig g;
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.11); });
    g.dummy(2);
    g.seg(0, R, 1.f, 0.5f); g.seg(1, R, T(1.0f), 0.5f);
    g.st->setParam(Stages::kRange + 0, 0.f);
    check("Faixa rápida compila", g.run(1.3));
    check("Ataque de 2,2 s: meio em 1,2 s", near(g.at(0, 1.2), 0.5f, 0.03f));
  }
  {  // Rampa acompanha o nível do segmento anterior (pseudo-VCA)
    Rig g;
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.5); });
    g.cv(1, [](double t) { return t < 0.7 ? 0.8f : 0.2f; });
    g.dummy(3);
    g.seg(0, R, T(0.01f), 0.5f); g.seg(1, H, 0.f, 0.5f); g.seg(2, R, T(0.2f), 0.5f);
    g.loop(1, 1);
    check("Tracking compila", g.run(1.0));
    check("Release acompanha o hold (0,2 V*0,5)", near(g.at(0, 0.716), 0.1f, 0.03f));
  }
  {  // Gate probabilístico: o pot é a probabilidade
    float peak[2];
    for (int variant = 0; variant < 2; ++variant) {
      Rig g;
      g.gate(5, [](double t) { return pulse(t, 0.1, 0.2); });
      g.seg(5, H, 0.6f, variant == 0 ? 0.f : 1.f);
      g.loop(5, 5);
      g.run(0.3);
      peak[variant] = g.maxOf(5);
    }
    check("Probabilidade 0: nunca dispara", near(peak[0], 0.f, 0.0001f));
    check("Probabilidade 1: sempre dispara", near(peak[1], 0.6f, 0.001f));
  }

  // ---- modo avançado ----
  {  // Slew com subida e descida independentes + retificação
    Rig g;
    g.st->setParam(Stages::kMode, 1);
    g.cv(0, [](double t) { return (t >= 0.1 && t < 0.5) ? 0.8f : 0.f; });
    g.seg(0, R, T(0.5f), T(0.02f));  // slider = descida (0,5 s), pot = subida (20 ms)
    g.cv(1, [](double) { return -0.6f; });
    g.seg(1, R, T(0.02f), T(0.02f));
    check("Slew compila", g.run(1.6));
    check("Slew: sobe rápido", g.at(0, 0.3) > 0.78f);
    check("Slew: desce com a constante da descida", near(g.at(0, 0.6), 0.8f * std::exp(-0.2f), 0.04f));
    check("Slew: cauda da descida", near(g.at(0, 1.5), 0.8f * std::exp(-2.f), 0.03f));
    check("Retificação em modo unipolar", near(g.at(1, 1.0), 0.6f, 0.02f));
  }
  {  // Atenuverter: step em loop atenua em vez de deslizar
    Rig g;
    g.st->setParam(Stages::kMode, 1);
    g.cv(0, [](double) { return 0.8f; });
    g.cv(1, [](double) { return 0.8f; });
    g.seg(0, S, 0.f, 0.5f); g.loop(0, 0);
    g.seg(1, S, 0.5f, 0.f); g.loop(1, 1); g.st->setParam(Stages::kBipolar + 1, 1.f);
    check("Atenuverter compila", g.run(0.2));
    check("Atenua: 0,5 x 0,8 = 0,4", near(g.at(0, 0.1), 0.4f, 0.01f));
    check("Atenuverte: -1 x 0,8 = -0,8", near(g.at(1, 0.1), -0.8f, 0.01f));
  }
  {  // Registrador de deslocamento (Turing Machine): trava (p=0) e inverte (p=1)
    for (int variant = 0; variant < 2; ++variant) {
      Rig g;
      g.st->seed(12345u);
      g.st->setParam(Stages::kMode, 1);
      g.gate(5, [](double t) { return std::fmod(t, 0.01) < 0.005 ? 1.f : 0.f; });
      g.seg(5, X, variant == 0 ? 0.f : 1.f, 1.f);  // slider = prob. de inverter, pot = 16 passos
      g.run(0.75);
      std::vector<float> s;
      for (int k = 0; k < 64; ++k) s.push_back(g.at(5, k * 0.01 + 0.007));
      bool period16 = true, comp16 = true, distinct = false;
      for (int k = 1; k < 32; ++k) {
        period16 = period16 && near(s[k + 16], s[k], 0.0001f);
        comp16 = comp16 && near(s[k + 16], 1.f - s[k], 0.0001f);
      }
      for (int k = 1; k < 16; ++k) distinct = distinct || !near(s[k], s[1], 0.0001f);
      if (variant == 0) {
        check("TM travada: repete a cada 16 passos", period16);
        check("TM: sequência não é constante", distinct);
      } else {
        check("TM com prob. 1: depois de 16 passos o valor é invertido", comp16);
      }
    }
  }
  {  // LFO aleatório em degraus (pot = 0), ~8 Hz
    Rig g;
    g.st->seed(777u);
    g.st->setParam(Stages::kMode, 1);
    g.seg(0, X, std::log(8.f / 0.125f) / std::log(256.f), 0.f);
    g.loop(0, 0);
    check("LFO aleatório compila", g.run(2.0));
    int changes = 0;
    const auto& v = g.pr->rec[0];
    for (size_t i = 1; i < v.size(); ++i) if (std::fabs(v[i] - v[i - 1]) > 0.001f) ++changes;
    char d[48];
    std::snprintf(d, sizeof(d), "(%d trocas em 2 s)", changes);
    check("LFO aleatório muda ~16 vezes em 2 s", changes >= 12 && changes <= 20, d);
    check("LFO aleatório fica entre 0 e 8 V", g.minOf(0) >= 0.f && g.maxOf(0) <= 1.f);
  }
  {  // Atrator caótico: limitado, finito e variando
    Rig g;
    g.st->setParam(Stages::kMode, 1);
    g.seg(0, X, 0.5f, 0.5f);
    check("Caos compila", g.run(6.0));
    double mean = 0, var = 0;
    bool finite = true;
    const auto& v = g.pr->rec[0];
    for (float x : v) { mean += x; finite = finite && std::isfinite(x); }
    mean /= v.size();
    for (float x : v) var += (x - mean) * (x - mean);
    var /= v.size();
    check("Caos: saída finita e entre 0 e 8 V", finite && g.minOf(0) >= 0.f && g.maxOf(0) <= 1.f);
    check("Caos: varia ao longo do tempo", std::sqrt(var) > 0.05);
  }

  // ======================= Ouroboros (oscilador harmônico) =======================
  {  // Raiz em 440 Hz (faixa alta, seno)
    Rig g;
    harmSetup(g, 4, rootX(440.f));
    check("Ouroboros compila", g.run(1.2));
    const int c = crossings(g, 0, 0.1, 1.1);
    char d[40]; std::snprintf(d, sizeof(d), "(%d ciclos)", c);
    check("Raiz em 440 Hz", c >= 438 && c <= 442, d);
    check("Raiz sozinha tem amplitude 1", near(g.maxOf(0), 1.f, 0.02f));
  }
  {  // Parcial 1 com razão 2x (faixa média, pot em 4/9)
    Rig g;
    harmSetup(g, 4, rootX(200.f));
    g.st->setParam(Stages::kSlider + 1, 1.f);
    g.st->setParam(Stages::kPot + 1, 4.f / 9.f);
    g.run(1.2);
    const int c = crossings(g, 1, 0.1, 1.1);
    char d[40]; std::snprintf(d, sizeof(d), "(%d ciclos)", c);
    check("Parcial na razão 2x = 400 Hz", c >= 398 && c <= 402, d);
  }
  {  // CV de 1 V/oct dobra a frequência da raiz
    Rig g;
    harmSetup(g, 4, rootX(220.f));
    g.cv(0, [](double) { return 1.f; });
    g.run(1.2);
    const int c = crossings(g, 0, 0.1, 1.1);
    check("CV de +1 oitava: 220 -> 440 Hz", c >= 438 && c <= 442);
  }
  {  // Formas de onda: serra e quadrada bandlimitadas
    Rig g;
    harmSetup(g, 4, rootX(1000.f));
    g.st->setParam(Stages::kWave + 0, 3.f);
    g.run(0.6);
    double mean = 0; size_t n0 = static_cast<size_t>(0.1 * kSr), n1 = g.pr->rec[0].size();
    for (size_t i = n0; i < n1; ++i) mean += g.pr->rec[0][i];
    mean /= static_cast<double>(n1 - n0);
    check("Serra: média ~0 e pico controlado", std::fabs(mean) < 0.02 && g.maxOf(0) < 1.2f && g.minOf(0) > -1.2f);
    check("Serra: ~1000 Hz", crossings(g, 0, 0.1, 0.6) >= 498 && crossings(g, 0, 0.1, 0.6) <= 502);
    Rig q;
    harmSetup(q, 4, rootX(1000.f));
    q.st->setParam(Stages::kWave + 0, 2.f);
    q.run(0.6);
    int flat = 0, tot = 0;
    for (size_t i = n0; i < q.pr->rec[0].size(); ++i, ++tot) if (std::fabs(q.pr->rec[0][i]) > 0.9f) ++flat;
    check("Quadrada: quase sempre em ±1", flat > 0.85 * tot);
  }
  {  // Strum: o gate sobe o volume do parcial no mix
    Rig g;
    harmSetup(g, 4, rootX(200.f));
    g.st->setParam(Stages::kPot + 1, 3.f / 9.f);  // razão 1,5 (300 Hz)
    g.gate(1, [](double t) { return pulse(t, 0.3, 0.31); });
    g.run(0.6);
    const double before = std::fabs(corr(g, 0, 1, 0.1, 0.28)), after = corr(g, 0, 1, 0.3, 0.42);
    check("Strum: parcial entra no mix só depois do gate", after > 0.03 && after > 5.0 * before);
  }
  {  // Faixa de LFO: raiz de 2 Hz
    Rig g;
    harmSetup(g, 4, 0.5f);
    g.st->setParam(Stages::kHarmRootRange, 1.f);
    g.run(4.2);
    const int c = crossings(g, 0, 0.1, 4.1);
    char d[40]; std::snprintf(d, sizeof(d), "(%d ciclos em 4 s)", c);
    check("Raiz em faixa de LFO: 2 Hz", c >= 7 && c <= 9, d);
  }
  {  // Em LFO, o gate reseta a fase e o volume atenua a saída individual
    Rig g;
    harmSetup(g, 4, 0.5f);
    g.st->setParam(Stages::kHarmRootRange, 1.f);
    g.st->setParam(Stages::kSlider + 1, 0.5f);
    g.st->setParam(Stages::kPot + 1, 2.f / 9.f);  // razão 1x
    g.gate(1, [](double t) { return pulse(t, 0.3, 0.31) + pulse(t, 0.9, 0.91); });
    g.run(1.2);
    check("LFO: gate reseta a fase (1º gate)", std::fabs(g.at(1, 0.301)) < 0.05f);
    check("LFO: gate reseta a fase (2º gate)", std::fabs(g.at(1, 0.901)) < 0.05f);
    check("LFO: continua depois do reset", near(g.at(1, 0.45), 0.5f * std::sin(2.f * 3.14159265f * 2.f * 0.15f), 0.04f));
    check("LFO: volume atenua a saída individual", g.maxOf(1) < 0.52f);
  }
  {  // Em LFO, cabo fantasma no gate tira o parcial do mix
    float c[2];
    for (int v = 0; v < 2; ++v) {
      Rig g;
      harmSetup(g, 4, 0.5f);
      g.st->setParam(Stages::kHarmRootRange, 1.f);
      g.st->setParam(Stages::kSlider + 1, 1.f);
      g.st->setParam(Stages::kPot + 1, 3.f / 9.f);  // razão 1,5 (3 Hz)
      if (v == 1) g.dummy(1);
      g.run(4.2);
      c[v] = static_cast<float>(corr(g, 0, 1, 0.2, 4.1));
    }
    check("LFO: sem cabo, o parcial está no mix", c[0] > 0.05f);
    check("LFO: cabo fantasma remove o parcial do mix", std::fabs(c[1]) < 0.01f);
  }
  {  // Modo 5: razão pelo slider+CV, volume pelo pot
    Rig g;
    harmSetup(g, 5, rootX(200.f));
    g.st->setParam(Stages::kPot + 1, 1.f);
    g.cv(1, [](double) { return 4.f / 9.f; });
    g.run(1.2);
    const int c = crossings(g, 1, 0.1, 1.1);
    char d[40]; std::snprintf(d, sizeof(d), "(%d ciclos)", c);
    check("Modo 5: CV escolhe a razão 2x (400 Hz)", c >= 398 && c <= 402, d);
  }
  {  // Modo 5: o pot é o volume no mix (a ausência de volume tira o parcial do mix)
    float c[2];
    for (int v = 0; v < 2; ++v) {
      Rig g;
      harmSetup(g, 5, rootX(200.f));
      g.st->setParam(Stages::kPot + 1, v == 0 ? 0.f : 1.f);
      g.cv(1, [](double) { return 3.f / 9.f; });  // razão 1,5 (300 Hz)
      g.run(1.2);
      c[v] = static_cast<float>(corr(g, 0, 1, 0.1, 1.1));
    }
    check("Modo 5: pot em 0 tira o parcial do mix", std::fabs(c[0]) < 0.01f);
    check("Modo 5: pot em 1 põe o parcial no mix", c[1] > 0.05f);
  }

  {  // Razões harmônicas e zona de snap
    check("Razão média no índice 4 = 2x", near(Stages::harmRatio(1, 4.f / 9.f), 2.f, 0.0001f));
    check("Snap: pequeno desvio mantém 2x", near(Stages::harmRatio(1, 4.f / 9.f + 0.02f / 9.f), 2.f, 0.0001f));
    check("Entre 2x e 3x desliza (média geométrica)", near(Stages::harmRatio(1, 4.5f / 9.f), std::sqrt(6.f), 0.01f));
    check("Faixa alta: 1x a 16x", near(Stages::harmRatio(2, 0.f), 1.f, 0.0001f) && near(Stages::harmRatio(2, 1.f), 16.f, 0.0001f));
    check("Faixa baixa: 1/16 a 1x", near(Stages::harmRatio(0, 0.f), 1.f / 16.f, 0.0001f) && near(Stages::harmRatio(0, 1.f), 1.f, 0.0001f));
  }

  // ======================= LEDs do painel =======================
  {  // Cores por tipo de segmento: verde (ramp), laranja (step), vermelho (hold)
    Rig g;
    g.seg(0, R, 0.5f, 0.5f); g.seg(1, S, 0.5f, 0.f); g.seg(2, H, 0.5f, 0.5f);
    check("LEDs compilam", g.run(0.3));
    auto a = g.ledAt(0, 0.2), b = g.ledAt(1, 0.2), c = g.ledAt(2, 0.2);
    check("LED do ramp é verde", a[1] > 0.1f && a[0] < 0.01f);
    check("LED do step é laranja", b[0] > 1.8f * b[1] && b[1] > 0.1f);
    check("LED do hold é vermelho", c[0] > 0.1f && c[1] < 0.01f);
  }
  {  // O brilho acompanha o sinal (decaimento de 0,4 s)
    Rig g;
    g.gate(5, [](double t) { return pulse(t, 0.1, 0.11); });
    g.seg(5, R, T(0.4f), 0.5f);
    g.run(0.8);
    check("LED brilha no início do decaimento", g.lumAt(5, 0.105) > 0.9f);
    check("LED cai junto com a saída (meio)", near(g.lumAt(5, 0.3), 0.6f, 0.08f));
    check("LED volta ao brilho de repouso", near(g.lumAt(5, 0.7), 0.2f, 0.03f));
  }
  {  // Loop pisca (step: pisca seco); sem loop o brilho é constante
    float mn[2], mx[2];
    for (int v = 0; v < 2; ++v) {
      Rig g;
      g.seg(0, S, 0.5f, 0.f);
      if (v == 1) g.loop(0, 0);
      g.run(3.3);
      mn[v] = 9.f; mx[v] = 0.f;
      for (double t = 0.2; t < 3.2; t += 0.002) { mn[v] = std::min(mn[v], g.lumAt(0, t)); mx[v] = std::max(mx[v], g.lumAt(0, t)); }
    }
    check("Sem loop: brilho constante", mx[0] - mn[0] < 0.02f);
    check("Com loop: o LED pisca", mn[1] < 0.05f && mx[1] > 0.5f);
  }
  {  // Loop de vários segmentos: primeiro e último piscam em "ping pong"
    Rig g;
    g.gate(0, [](double) { return 0.f; });
    g.dummy(2);
    g.seg(0, R, 0.5f, 0.5f); g.seg(1, R, 0.5f, 0.5f);
    g.loop(0, 1);
    g.run(3.3);
    double m0 = 0, m1 = 0; int n = 0;
    for (double t = 0.2; t < 3.2; t += 0.002, ++n) { m0 += g.lumAt(0, t); m1 += g.lumAt(1, t); }
    m0 /= n; m1 /= n;
    double c01 = 0, v0 = 0, v1 = 0;
    for (double t = 0.2; t < 3.2; t += 0.002) {
      const double a = g.lumAt(0, t) - m0, b = g.lumAt(1, t) - m1;
      c01 += a * b; v0 += a * a; v1 += b * b;
    }
    char d[40]; std::snprintf(d, sizeof(d), "(correlação %.2f)", c01 / std::sqrt(v0 * v1));
    check("Ping pong: LEDs do início e do fim piscam em oposição", c01 / std::sqrt(v0 * v1) < -0.8, d);
  }
  {  // Bipolar: piscada vermelha fraca 1x por segundo
    float frac[2];
    for (int v = 0; v < 2; ++v) {
      Rig g;
      g.seg(0, R, 0.5f, 0.5f);
      if (v == 1) g.st->setParam(Stages::kBipolar + 0, 1.f);
      g.run(3.1);
      int on = 0, tot = 0;
      for (double t = 0.05; t < 3.0; t += 0.002, ++tot) if (g.ledAt(0, t)[0] > 0.2f) ++on;
      frac[v] = static_cast<float>(on) / static_cast<float>(tot);
    }
    check("Sem bipolar: sem piscada vermelha", frac[0] < 0.001f);
    check("Bipolar: piscada vermelha ~10% do tempo", near(frac[1], 0.1f, 0.04f));
  }
  {  // DAHDSR idênticos: cor por fase (verde, laranja no sustain, vermelho no release, apagado em repouso)
    Rig g;
    g.st->setParam(Stages::kMode, 3);
    g.gate(0, [](double t) { return pulse(t, 0.1, 0.8); });
    const float v[6] = {ds(0.1f), ds(0.05f), ds(0.1f), ds(0.1f), 0.5f, ds(0.2f)};
    for (int k = 0; k < 6; ++k) g.st->setParam(Stages::kSlider + k, v[k]);
    g.run(1.3);
    auto delay = g.ledAt(0, 0.15), hold = g.ledAt(0, 0.3), sus = g.ledAt(0, 0.6), rel = g.ledAt(0, 0.9);
    check("DAHDSR LED: delay e hold em verde", delay[1] > 0.9f && delay[0] < 0.05f && hold[1] > 0.9f);
    check("DAHDSR LED: sustain em laranja", sus[0] > 0.9f && sus[1] > 0.3f && sus[1] < 0.6f);
    check("DAHDSR LED: release em vermelho", rel[0] > 0.9f && rel[1] < 0.05f);
    check("DAHDSR LED: apagado em repouso", g.lumAt(0, 1.2) < 0.01f && g.lumAt(1, 0.6) < 0.01f);
  }
  {  // DAHDSR independentes: o selecionado fica amarelo em repouso
    Rig g;
    g.st->setParam(Stages::kMode, 2);
    g.st->setParam(Stages::kEnvSelected, 2.f);
    g.run(0.2);
    auto sel = g.ledAt(2, 0.1);
    check("Independente: selecionado em repouso é amarelo", sel[0] > 0.9f && sel[1] > 0.7f);
    check("Independente: os outros ficam apagados", g.lumAt(0, 0.1) < 0.01f && g.lumAt(5, 0.1) < 0.01f);
  }
  {  // Ouroboros: cor mostra a forma de onda (seno, triângulo, quadrada; serra pisca)
    Rig g;
    harmSetup(g, 4, rootX(200.f));
    g.st->setParam(Stages::kWave + 0, 0.f); g.st->setParam(Stages::kWave + 1, 1.f);
    g.st->setParam(Stages::kWave + 2, 2.f); g.st->setParam(Stages::kWave + 3, 3.f);
    g.run(1.2);
    auto a = g.ledAt(0, 0.2), b = g.ledAt(1, 0.2), c = g.ledAt(2, 0.2);
    check("Ouroboros LED: seno verde, triângulo laranja, quadrada vermelha",
          a[1] > 0.9f && a[0] < 0.01f && b[0] > 1.8f * b[1] && b[1] > 0.05f && c[0] > 0.3f && c[1] < 0.01f);
    float mn = 9.f, mx = 0.f;
    for (double t = 0.1; t < 1.1; t += 0.002) { mn = std::min(mn, g.lumAt(3, t)); mx = std::max(mx, g.lumAt(3, t)); }
    check("Ouroboros LED: serra pisca", mn < 0.5f * mx);
  }
  {  // Modo daltônico: step mais brilhante que ramp, e ramp mais que hold
    Rig g;
    g.st->setParam(Stages::kColorBlind, 1.f);
    g.seg(0, R, 0.5f, 0.5f); g.seg(1, S, 0.5f, 0.f); g.seg(2, H, 0.5f, 0.5f);
    g.run(0.3);
    const float r = g.lumAt(0, 0.2), st = g.lumAt(1, 0.2), h = g.lumAt(2, 0.2);
    check("Modo daltônico: step > ramp > hold", st > r && r > h);
  }

  // ======================= Sequenciador estendido (firmware oficial 1.2) =======================
  {
    const float sl[4] = {0.f, 3.f / 12.f, 7.f / 12.f, 1.f};          // 0, 3, 7 e 12 semitons
    const float lv[4] = {0.f, 3.f / 96.f, 7.f / 96.f, 12.f / 96.f};  // saída em unidades de 8 V
    auto seq = [&](float pot, int n) {
      std::vector<int> r; for (float v : extSeq(Stages::kRamp, pot, sl, n)) r.push_back(stepIndex(v, lv)); return r; };
    auto same = [](const std::vector<int>& a, const std::vector<int>& b) { return a == b; };
    check("Seq: para cima", same(seq(0.05f, 5), {0, 1, 2, 3, 0}));
    check("Seq: para baixo", same(seq(0.2f, 5), {3, 2, 1, 0, 3}));
    check("Seq: pêndulo", same(seq(0.35f, 8), {0, 1, 2, 3, 2, 1, 0, 1}));
    check("Seq: alternado (1, 2, 1, 3, 1, 4, 1, 3, 1, 2)", same(seq(0.5f, 10), {0, 1, 0, 2, 0, 3, 0, 2, 0, 1}));
    {
      auto r = seq(0.64f, 40);
      bool all[4] = {false, false, false, false}, rep = false;
      for (size_t i = 0; i < r.size(); ++i) { if (r[i] >= 0) all[r[i]] = true; if (i && r[i] == r[i - 1]) rep = true; }
      check("Seq: aleatório usa os 4 passos e pode repetir", all[0] && all[1] && all[2] && all[3] && rep);
    }
    {
      auto r = seq(0.78f, 60);
      bool all[4] = {false, false, false, false}, noRep = true;
      for (size_t i = 0; i < r.size(); ++i) { if (r[i] >= 0) all[r[i]] = true; if (i && r[i] == r[i - 1]) noRep = false; }
      check("Seq: pseudoaleatório nunca repete e usa os 4 passos", all[0] && all[1] && all[2] && all[3] && noRep);
    }
    {  // endereçável: slider+CV do primeiro segmento escolhem o passo; o clock é ignorado
      Rig g;
      g.gate(0, [](double t) { return std::fmod(t, 0.1) >= 0.05 && std::fmod(t, 0.1) < 0.07 ? 1.f : 0.f; });
      g.cv(0, [](double t) { return t < 0.5 ? 0.6f : 0.1f; });
      g.dummy(5);
      g.seg(0, Stages::kRamp, 0.f, 0.93f);
      for (int i = 0; i < 4; ++i) g.seg(1 + i, Stages::kStep, sl[i], 0.f);
      g.run(1.0);
      check("Seq: endereçável escolhe o passo pela CV", stepIndex(g.at(0, 0.3), lv) == 2 && stepIndex(g.at(0, 0.8), lv) == 0);
      check("Seq: endereçável ignora o clock", stepIndex(g.at(0, 0.41), lv) == 2 && stepIndex(g.at(0, 0.46), lv) == 2);
    }
    {  // reset, inibição de 5 ms e clock inibido com o reset alto
      auto build = [&](std::function<float(double)> reset) {
        auto g = std::make_unique<Rig>();
        g->gate(0, [](double t) { return std::fmod(t, 0.1) >= 0.05 && std::fmod(t, 0.1) < 0.07 ? 1.f : 0.f; });
        g->cv(0, reset);
        g->dummy(5);
        g->seg(0, Stages::kRamp, 0.f, 0.05f);
        for (int i = 0; i < 4; ++i) g->seg(1 + i, Stages::kStep, sl[i], 0.f);
        g->run(0.9);
        return g;
      };
      auto g1 = build([](double t) { return pulse(t, 0.30, 0.31) * 0.8f; });
      check("Seq: reset volta ao primeiro passo", stepIndex(g1->at(0, 0.28), lv) == 2 && stepIndex(g1->at(0, 0.32), lv) == 0);
      check("Seq: o clock seguinte avança do passo 1 para o 2", stepIndex(g1->at(0, 0.38), lv) == 1 && stepIndex(g1->at(0, 0.48), lv) == 2);
      auto g2 = build([](double t) { return pulse(t, 0.347, 0.348) * 0.8f; });
      check("Seq: clock até 5 ms depois do reset é inibido", stepIndex(g2->at(0, 0.40), lv) == 0);
      auto g3 = build([](double t) { return pulse(t, 0.30, 0.50) * 0.8f; });
      check("Seq: clock inibido enquanto o reset está alto", stepIndex(g3->at(0, 0.48), lv) == 0);
      check("Seq: volta a avançar depois que o reset cai", stepIndex(g3->at(0, 0.58), lv) == 1);
    }
    {
      auto r = extSeq(Stages::kRamp, 0.05f, sl, 6, true);  // loop nos segmentos 3 e 4 = passos 2 e 3
      std::vector<int> idx; for (float v : r) idx.push_back(stepIndex(v, lv));
      check("Seq: o loop restringe a reprodução aos passos em loop", same(idx, {1, 2, 1, 2, 1, 2}));
    }
    {  // primeiro segmento HOLD: sem quantização, 0 a 8 V
      const float raw[4] = {0.3f, 0.7f, 0.5f, 0.9f};
      auto h = extSeq(Stages::kHold, 0.05f, raw, 2), q = extSeq(Stages::kRamp, 0.05f, raw, 2);
      check("Seq: primeiro HOLD não quantiza", near(h[0], 0.3f, 0.0006f) && near(h[1], 0.7f, 0.0006f));
      check("Seq: primeiro RAMP quantiza em semitons (0,3 -> 4/96)", near(q[0], 4.f / 96.f, 0.0006f));
    }
    {  // saídas 2 a N mostram o passo ativo
      Rig g;
      g.gate(0, [](double t) { return std::fmod(t, 0.1) >= 0.05 && std::fmod(t, 0.1) < 0.07 ? 1.f : 0.f; });
      g.dummy(5);
      g.seg(0, Stages::kRamp, 0.f, 0.05f);
      for (int i = 0; i < 4; ++i) g.seg(1 + i, Stages::kStep, sl[i], 0.f);
      g.run(0.5);
      check("Seq: saída do passo ativo = 1 e as demais = 0", near(g.at(3, 0.29), 1.f, 0.001f) && near(g.at(2, 0.29), 0.f, 0.001f));
    }
  }
  {  // LEDs de saída (ao lado de cada jack): seguem o nível da saída
    Rig g;
    g.gate(5, [](double t) { return pulse(t, 0.1, 0.11); });
    g.seg(5, R, T(0.4f), 0.5f);
    g.run(0.8);
    check("LED de saída acompanha a saída (meio do decaimento)", near(g.lumAt(11, 0.3), std::pow(0.5f, 0.7f), 0.1f));
    check("LED de saída apaga em repouso", g.lumAt(11, 0.7) < 0.01f);
    check("LED de saída tem a cor do tipo (verde)", g.ledAt(11, 0.2)[1] > 0.1f && g.ledAt(11, 0.2)[0] < 0.01f);
  }

  std::printf("\n%s (%d falha(s))\n", failures == 0 ? "TODOS OS TESTES PASSARAM" : "HÁ FALHAS", failures);
  return failures == 0 ? 0 : 1;
}
