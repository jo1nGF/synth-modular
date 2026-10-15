// SONDA DO CÓDIGO ORIGINAL (firmware do qiemem, derivado da Mutable Instruments, licença MIT).
//
// Compila o SegmentGenerator REAL e mede os mapeamentos que eu tinha suposto: tempo de rampa x slider, curva x
// pot, frequência do LFO, glide do step, duração do pulso e um ADSR completo. O resultado é impresso em texto
// e serve para corrigir as aproximações marcadas [ASSUMIDO] em src/stages.h.
//
// Só roda no GitHub Actions (job "sonda-original"), dentro da pasta do repositório original:
//   g++ -DTEST -msse2 -O2 -I. <fontes do makefile de stages/test> original_probe.cc

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "stages/segment_generator.h"

using namespace stages;
using namespace stmlib;

namespace {

const size_t kBlk = 8;           // tamanho de bloco do firmware
const double kSr = 31250.0;      // taxa de amostragem do firmware (stages::kSampleRate)

struct Runner {
  SegmentGenerator g;
  GateFlags prev;
  Runner() : prev(GATE_FLAG_LOW) { g.Init(); }

  // gate(i): gate alto na amostra i. params(g, i): chamado a cada bloco para atualizar os parâmetros.
  std::vector<float> run(size_t n, const std::function<bool(size_t)>& gate,
                         const std::function<void(SegmentGenerator&, size_t)>& params) {
    std::vector<float> v;
    v.reserve(n);
    GateFlags flags[kBlk];
    SegmentGenerator::Output out[kBlk];
    for (size_t i = 0; i < n; i += kBlk) {
      params(g, i);
      for (size_t k = 0; k < kBlk; ++k) {
        prev = ExtractGateFlags(prev, gate(i + k));
        flags[k] = prev;
      }
      g.Process(flags, out, kBlk);
      for (size_t k = 0; k < kBlk; ++k) v.push_back(out[k].value);
    }
    return v;
  }
};

bool pulse(size_t i) { return i >= 80 && i < 120; }  // gate curto no começo

// ---- 1) Decaimento (ramp com gate): duração x slider (primary) e curva x pot (secondary) -------------------
void probeDecay() {
  std::printf("\n[1] RAMP isolado com gate (decaimento): duração até cair a 0,001 e fração do tempo em que cruza 0,5\n");
  std::printf("    primary  shape  duração(s)  fração_em_0.5\n");
  const float prim[] = {0.f, 0.1f, 0.25f, 0.4f, 0.5f, 0.6f, 0.75f, 0.9f, 1.f};
  const float shp[] = {0.5f};
  for (float p : prim) {
    for (float s : shp) {
      Runner r;
      segment::Configuration c = {segment::TYPE_RAMP, false};
      r.g.Configure(true, &c, 1);
      auto v = r.run(static_cast<size_t>(45 * kSr), pulse, [&](SegmentGenerator& g, size_t) { g.set_segment_parameters(0, p, s); });
      size_t i0 = 0, iend = 0, ihalf = 0;
      for (size_t i = 80; i < v.size(); ++i) if (v[i] > 0.5f) { i0 = i; break; }
      for (size_t i = i0; i < v.size(); ++i) { if (!ihalf && v[i] <= 0.5f) ihalf = i; if (v[i] < 0.001f) { iend = i; break; } }
      const double dur = iend ? (iend - i0) / kSr : -1.0;
      std::printf("    %.2f     %.2f   %9.4f   %.3f\n", p, s, dur, (iend && ihalf) ? double(ihalf - i0) / double(iend - i0) : -1.0);
    }
  }
  std::printf("    curva (primary 0,5): fração do tempo em que a saída cruza 0,5, por valor do pot\n");
  for (float s : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
    Runner r;
    segment::Configuration c = {segment::TYPE_RAMP, false};
    r.g.Configure(true, &c, 1);
    auto v = r.run(static_cast<size_t>(8 * kSr), pulse, [&](SegmentGenerator& g, size_t) { g.set_segment_parameters(0, 0.5f, s); });
    size_t i0 = 0, iend = 0, ihalf = 0;
    for (size_t i = 80; i < v.size(); ++i) if (v[i] > 0.5f) { i0 = i; break; }
    for (size_t i = i0; i < v.size(); ++i) { if (!ihalf && v[i] <= 0.5f) ihalf = i; if (v[i] < 0.001f) { iend = i; break; } }
    std::printf("      pot=%.2f -> duração %.4f s, cruza 0,5 em %.3f do tempo\n", s, iend ? (iend - i0) / kSr : -1.0,
                (iend && ihalf) ? double(ihalf - i0) / double(iend - i0) : -1.0);
  }
}

// ---- 2) LFO livre: frequência x slider --------------------------------------------------------------------
void probeLfo() {
  std::printf("\n[2] RAMP em loop sem gate (LFO livre): frequência por valor do slider (secondary = 0,5)\n");
  for (float p : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
    Runner r;
    segment::Configuration c = {segment::TYPE_RAMP, true};
    r.g.Configure(false, &c, 1);
    auto v = r.run(static_cast<size_t>(40 * kSr), [](size_t) { return false; }, [&](SegmentGenerator& g, size_t) { g.set_segment_parameters(0, p, 0.5f); });
    float mn = 9.f, mx = -9.f;
    for (size_t i = static_cast<size_t>(kSr); i < v.size(); ++i) { mn = std::min(mn, v[i]); mx = std::max(mx, v[i]); }
    const float mid = 0.5f * (mn + mx);
    size_t first = 0, last = 0; int count = 0;
    for (size_t i = static_cast<size_t>(kSr) + 1; i < v.size(); ++i)
      if (v[i - 1] < mid && v[i] >= mid) { if (!count) first = i; last = i; ++count; }
    std::printf("    primary=%.2f -> %s  (min %.2f, max %.2f)\n", p,
                count >= 2 ? "freq" : "<2 ciclos", mn, mx);
    if (count >= 2) std::printf("                  %.4f Hz\n", (count - 1) / ((last - first) / kSr));
  }
}

// ---- 3) STEP sem gate (portamento): constante de tempo x pot ----------------------------------------------
void probePortamento() {
  std::printf("\n[3] STEP livre (portamento): tempo para chegar a 63%% de um degrau de 0,2 para 0,8, por valor do pot\n");
  for (float s : {0.f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.f}) {
    Runner r;
    segment::Configuration c = {segment::TYPE_STEP, false};
    r.g.Configure(false, &c, 1);
    const size_t step = static_cast<size_t>(1 * kSr);
    auto v = r.run(static_cast<size_t>(40 * kSr), [](size_t) { return false; },
                   [&](SegmentGenerator& g, size_t i) { g.set_segment_parameters(0, i < step ? 0.2f : 0.8f, s); });
    const float target = 0.2f + 0.63f * 0.6f;
    double t63 = -1.0;
    for (size_t i = step; i < v.size(); ++i) if (v[i] >= target) { t63 = (i - step) / kSr; break; }
    std::printf("    pot=%.2f -> %.4f s  (valor final %.3f)\n", s, t63, v.back());
  }
}

// ---- 4) HOLD com gate (gerador de pulso): duração x pot ----------------------------------------------------
void probePulse() {
  std::printf("\n[4] HOLD isolado com gate (pulso temporizado): duração por valor do pot (primary = 0,5)\n");
  for (float s : {0.f, 0.25f, 0.5f, 0.75f, 1.f}) {
    Runner r;
    segment::Configuration c = {segment::TYPE_HOLD, false};
    r.g.Configure(true, &c, 1);
    auto v = r.run(static_cast<size_t>(30 * kSr), pulse, [&](SegmentGenerator& g, size_t) { g.set_segment_parameters(0, 0.5f, s); });
    size_t n = 0; float peak = 0;
    for (float x : v) { if (x > 0.25f) ++n; peak = std::max(peak, x); }
    std::printf("    pot=%.2f -> %.4f s em nível alto (pico %.3f)\n", s, n / kSr, peak);
  }
}

// ---- 5) ADSR completo: [ramp][ramp][hold em loop][ramp] ---------------------------------------------------
void probeAdsr() {
  std::printf("\n[5] ADSR [ramp][ramp][hold+loop][ramp]: sliders 0,238 / 0,404 / 0,5 (nível) / 0,476; pots 0,5; gate de 0,1 a 0,6 s\n");
  Runner r;
  segment::Configuration c[4] = {{segment::TYPE_RAMP, false}, {segment::TYPE_RAMP, false}, {segment::TYPE_HOLD, true}, {segment::TYPE_RAMP, false}};
  r.g.Configure(true, c, 4);
  auto v = r.run(static_cast<size_t>(2 * kSr), [](size_t i) { return i >= 0.1 * kSr && i < 0.6 * kSr; },
                 [](SegmentGenerator& g, size_t) {
                   g.set_segment_parameters(0, 0.238f, 0.5f);
                   g.set_segment_parameters(1, 0.404f, 0.5f);
                   g.set_segment_parameters(2, 0.5f, 0.5f);
                   g.set_segment_parameters(3, 0.476f, 0.5f);
                 });
  float peak = 0; size_t ipk = 0;
  for (size_t i = 0; i < v.size(); ++i) if (v[i] > peak) { peak = v[i]; ipk = i; }
  std::printf("    pico %.3f em t=%.4f s\n", peak, ipk / kSr);
  for (double t : {0.09, 0.12, 0.15, 0.2, 0.3, 0.4, 0.55, 0.65, 0.8, 1.0, 1.5})
    std::printf("    t=%.2f s -> %.4f\n", t, v[static_cast<size_t>(t * kSr)]);
}

}  // namespace

int main() {
  std::printf("SONDA DO CODIGO ORIGINAL: stages::kSampleRate = %.0f Hz, bloco = %zu amostras\n", kSr, kBlk);
  probeDecay();
  probeLfo();
  probePortamento();
  probePulse();
  probeAdsr();
  std::printf("\nfim\n");
  return 0;
}
