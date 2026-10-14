#pragma once
// Adaptador do Plaits (firmware 1.2, pichenettes/eurorack, licença MIT).
//
// O uso da API foi conferido contra um UGen público de SuperCollider que usa o
// código original (gist sbl/976ddd42d61230831a154f7da2481051):
//   - #include "plaits/dsp/voice.h"
//   - stmlib::BufferAllocator + voice.Init(&allocator), buffer de 16384 bytes
//   - campos de Patch: engine, note, harmonics, timbre, morph, lpg_colour, decay,
//     frequency_modulation_amount
//   - campos de Modulations usados: trigger, trigger_patched
//   - Voice::Frame com out/aux em int16; Render(patch, modulations, frames, kBlockSize)
//   - patch.note = 60 + 12 * (V/oct), ou seja, 0 V = C4, igual ao nosso motor
// Ainda NÃO foi compilado contra os headers reais.

#ifdef WITH_MI_PLAITS

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine.h"
#include "plaits/dsp/voice.h"

namespace mod {

constexpr int kPlaitsBlock = static_cast<int>(plaits::kBlockSize);

// Entradas : 0 = V/oct, 1 = gate/trigger, 2 = harmonics CV, 3 = timbre CV, 4 = morph CV
// Saídas   : 0 = OUT, 1 = AUX
// LEDs     : 8 LEDs de modelo (coluna central do painel). Acende o LED engine % 8; a cor mostra o banco:
//            verde (engines 0 a 7), vermelho (8 a 15) e laranja (16 a 23, firmware 1.2).
// Params   : 0 = engine (inteiro), 1 = harmonics, 2 = timbre, 3 = morph,
//            4 = decay, 5 = lpg colour, 6 = quantidade de FM de frequência
//
// O Plaits foi feito para 48 kHz e blocos de plaits::kBlockSize frames (12).
// O motor deve usar block múltiplo de 12 (o demo usa 96). Em outra taxa de
// amostragem o pitch é compensado (mesmo truque do UGen), mas o timbre e os
// tempos mudam de forma audível.
class PlaitsModule : public Module {
 public:
  PlaitsModule() : Module(5, 2, 8, 8) {
    setParam(0, 0.f);
    setParam(1, 0.5f);
    setParam(2, 0.5f);
    setParam(3, 0.5f);
    setParam(4, 0.5f);
    setParam(5, 0.5f);
    setParam(6, 0.f);
  }

  void prepare(float sampleRate, int block) override {
    Module::prepare(sampleRate, block);
    ok_ = (block % kPlaitsBlock) == 0;
    pitchComp_ = std::log2(48000.f / sampleRate);

    std::memset(sharedBuffer_, 0, sizeof(sharedBuffer_));
    stmlib::BufferAllocator allocator(sharedBuffer_, sizeof(sharedBuffer_));
    voice_.Init(&allocator);

    std::memset(&patch_, 0, sizeof(patch_));
    std::memset(&modulations_, 0, sizeof(modulations_));
    patch_.engine = 0;
    patch_.lpg_colour = 0.5f;
    patch_.decay = 0.5f;
    modulations_.trigger_patched = true;
  }

  void process(int n) override {
    float* outMain = out(0);
    float* outAux = out(1);
    if (!ok_) {  // block incompatível: silêncio em vez de áudio quebrado
      std::fill(outMain, outMain + n, 0.f);
      std::fill(outAux, outAux + n, 0.f);
      return;
    }

    const int engine = std::max(0, std::min(23, static_cast<int>(param(0))));
    static const float bank[3][3] = {{0.f, 1.f, 0.f}, {1.f, 0.f, 0.f}, {1.f, 0.45f, 0.f}};
    for (int l = 0; l < 8; ++l) {
      const float on = (engine % 8 == l) ? 1.f : 0.f;
      setLed(l, bank[engine / 8][0] * on, bank[engine / 8][1] * on, bank[engine / 8][2] * on);
    }
    const float* vOct = in(0);
    const float* gate = in(1);
    const float* harmCv = in(2);
    const float* timbCv = in(3);
    const float* morphCv = in(4);

    for (int i = 0; i < n; i += kPlaitsBlock) {
      const int last = i + kPlaitsBlock - 1;  // CV amostrado 1x por bloco

      patch_.engine = static_cast<int>(param(0));
      patch_.harmonics = clamp01(param(1) + harmCv[last]);
      patch_.timbre = clamp01(param(2) + timbCv[last]);
      patch_.morph = clamp01(param(3) + morphCv[last]);
      patch_.decay = clamp01(param(4));
      patch_.lpg_colour = clamp01(param(5));
      patch_.frequency_modulation_amount = clamp01(param(6));
      patch_.note = 60.f + 12.f * (vOct[last] + pitchComp_);

      modulations_.trigger = gate[last] > 0.5f ? 1.f : 0.f;

      plaits::Voice::Frame frames[plaits::kBlockSize];
      voice_.Render(patch_, modulations_, frames, plaits::kBlockSize);

      for (int j = 0; j < kPlaitsBlock; ++j) {
        outMain[i + j] = frames[j].out / 32768.f;
        outAux[i + j] = frames[j].aux / 32768.f;
      }
    }
  }

 private:
  static float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

  plaits::Voice voice_;
  plaits::Patch patch_;
  plaits::Modulations modulations_;
  char sharedBuffer_[16384];
  float pitchComp_ = 0.f;
  bool ok_ = false;
};

}  // namespace mod

#endif  // WITH_MI_PLAITS
