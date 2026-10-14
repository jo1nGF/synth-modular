// Testa o patch do app Android no PC: g++ -std=c++17 -O2 -Isrc tests/test_patch.cpp -o test_patch && ./test_patch
#include <cmath>
#include <cstdio>
#include <vector>

#include "synth_patch.h"

using mod::SynthPatch;
static int failures = 0;
static void check(const char* n, bool ok) { std::printf("[%s] %s\n", ok ? "OK " : "FALHA", n); if (!ok) ++failures; }

static std::vector<float> renderSeconds(SynthPatch& p, double sec, int sr, int chunk) {
  std::vector<float> l(chunk), r(chunk), all;
  for (int done = 0; done < static_cast<int>(sec * sr); done += chunk) {
    p.render(l.data(), r.data(), chunk);
    all.insert(all.end(), l.begin(), l.end());
  }
  return all;
}
// Conta notas pelo LED da saída 1 do Stages (envelope): cada nota leva o envelope a 8 V.
static int notesByLed(SynthPatch& p, double sec, int sr) {
  std::vector<float> l(96), r(96);
  int n = 0; bool armed = true;
  for (int done = 0; done < static_cast<int>(sec * sr); done += 96) {
    p.render(l.data(), r.data(), 96);
    float rgb[3]; p.getLed(SynthPatch::kStages, 6, rgb);
    const float lum = std::max(rgb[0], std::max(rgb[1], rgb[2]));
    if (armed && lum > 0.97f) { ++n; armed = false; }
    if (lum < 0.9f) armed = true;
  }
  return n;
}
static float peak(const std::vector<float>& v) { float m = 0; for (float x : v) m = std::max(m, std::fabs(x)); return m; }

int main() {
  const int sr = 48000;
  SynthPatch p;
  check("build compila", p.build(sr));
  auto a = renderSeconds(p, 2.0, sr, 480);   // 480 não é múltiplo de 96? é (5 blocos)
  double e = 0; for (float x : a) e += x * x;
  check("áudio não é silêncio (RMS > 0,03)", std::sqrt(e / a.size()) > 0.03);
  check("pico respeita o volume (0,4)", peak(a) <= 0.4f * 1.01f && peak(a) > 0.2f);
  auto odd = renderSeconds(p, 1.0, sr, 441);  // callback de tamanho irregular
  check("tamanho irregular de callback funciona", peak(odd) > 0.1f && std::isfinite(peak(odd)));

  const int n3 = notesByLed(p, 2.0, sr);
  p.setParam(SynthPatch::kClock, 0, 6.f);
  const int n6 = notesByLed(p, 2.0, sr);
  char d[64]; std::snprintf(d, sizeof(d), "(3 Hz: %d notas, 6 Hz: %d notas em 2 s)", n3, n6);
  check("andamento: dobrar o clock dobra as notas", n3 >= 5 && n3 <= 7 && n6 >= 11 && n6 <= 13);
  std::printf("      %s\n", d);

  p.setMaster(0.2f);
  auto c = renderSeconds(p, 1.0, sr, 480);
  check("volume mestre escala o pico", peak(c) <= 0.2f * 1.01f && peak(c) > 0.08f);

  p.setParam(-1, 0, 1.f); p.setParam(99, 0, 1.f); p.setParam(SynthPatch::kStages, 99999, 1.f); p.setParam(SynthPatch::kClock, -5, 1.f);
  check("parâmetros inválidos são ignorados", true);

  check("Stages expõe 12 LEDs", p.numLeds(SynthPatch::kStages) == 12 && p.numLeds(SynthPatch::kClock) == 0);
  float rgb[3] = {-1, -1, -1};
  bool ok = true;
  for (int i = 0; i < 12; ++i) { ok = ok && p.getLed(SynthPatch::kStages, i, rgb) && std::isfinite(rgb[0] + rgb[1] + rgb[2]) && rgb[0] >= 0 && rgb[0] <= 1.01f; }
  check("LEDs legíveis e dentro de 0..1", ok && !p.getLed(SynthPatch::kStages, 12, rgb));

  check("rebuild em 44,1 kHz", p.build(44100) && peak(renderSeconds(p, 0.5, 44100, 441)) > 0.1f);
  p.release();
  check("release deixa o patch inativo", !p.ready());
  std::printf("\n%s (%d falha(s))\n", failures == 0 ? "TODOS OS TESTES PASSARAM" : "HÁ FALHAS", failures);
  return failures == 0 ? 0 : 1;
}
