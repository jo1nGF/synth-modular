// Teste no PC: monta um patch, renderiza 4 s em demo.wav e mede o custo de CPU.
//
//   Clock ──► Seq4 ──► SineOsc ──► Vca ──► Out (L e R)
//     └─────► AdEnv ───────────────┘

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "engine.h"
#include "modules.h"

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r,
                     int sampleRate) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  const uint32_t frames = static_cast<uint32_t>(l.size());
  const uint32_t dataBytes = frames * 2 * sizeof(int16_t);
  auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f);
  w32(36 + dataBytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  w32(16);
  w16(1);
  w16(2);
  w32(sampleRate);
  w32(sampleRate * 4);
  w16(4);
  w16(16);
  std::fwrite("data", 1, 4, f);
  w32(dataBytes);
  auto toS16 = [](float x) {
    x = x > 1.f ? 1.f : (x < -1.f ? -1.f : x);
    return static_cast<int16_t>(x * 32767.f);
  };
  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t s[2] = {toS16(l[i]), toS16(r[i])};
    std::fwrite(s, sizeof(s), 1, f);
  }
  std::fclose(f);
}

int main(int argc, char** argv) {
  const char* outPath = argc > 1 ? argv[1] : "demo.wav";
  const int sr = 48000;
  const int block = 96;  // 2 ms em 48 kHz; múltiplo de 12 (útil para o Plaits)

  mod::Engine eng(sr, block);
  auto* clock = eng.add<mod::Clock>();
  auto* seq = eng.add<mod::Seq4>();
  auto* osc = eng.add<mod::SineOsc>();
  auto* env = eng.add<mod::AdEnv>();
  auto* vca = eng.add<mod::Vca>();
  auto* out = eng.add<mod::AudioOut>();

  clock->setParam(0, 4.f);  // 4 notas por segundo
  clock->setParam(1, 0.3f);
  const float notes[4] = {0.f, 7.f, 12.f, 3.f};
  for (int i = 0; i < 4; ++i) seq->setParam(i, notes[i]);
  env->setParam(1, 0.2f);

  eng.connect(clock, 0, seq, 0);
  eng.connect(seq, 0, osc, 0);
  eng.connect(clock, 0, env, 0);
  eng.connect(osc, 0, vca, 0);
  eng.connect(env, 0, vca, 1);
  eng.connect(vca, 0, out, 0);
  eng.connect(vca, 0, out, 1);

  if (!eng.compile(out)) {
    std::fprintf(stderr, "erro: patch inválido (cabo ruim ou ciclo)\n");
    return 1;
  }

  const int seconds = 4;
  const int total = sr * seconds;
  std::vector<float> l(total), r(total);

  // Simula um callback de áudio com tamanho irregular (ex.: 192 e 240 frames).
  const auto t0 = std::chrono::steady_clock::now();
  int done = 0, k = 0;
  const int sizes[2] = {192, 240};
  while (done < total) {
    const int n = std::min(sizes[k++ & 1], total - done);
    eng.render(l.data() + done, r.data() + done, n);
    done += n;
  }
  const auto t1 = std::chrono::steady_clock::now();

  const double elapsed = std::chrono::duration<double>(t1 - t0).count();
  std::printf("renderizado %d s de áudio em %.4f s  (%.0fx tempo real)\n", seconds, elapsed,
              seconds / elapsed);

  double peak = 0;
  for (float x : l) peak = std::max(peak, static_cast<double>(std::fabs(x)));
  std::printf("pico: %.3f\n", peak);

  writeWav(outPath, l, r, sr);
  std::printf("gravado: %s\n", outPath);
  return 0;
}
