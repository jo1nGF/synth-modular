#pragma once
// Patch de demonstração para o app Android (e testável no PC):
//   Clock -> Seq4 -> SineOsc -> VCA -> saída estéreo
//   Clock -> Stages (ADSR de 4 segmentos) -> CV do VCA
// Sem nenhuma dependência de Android: o código de áudio e de JNI fica em native-lib.cpp.

#include <algorithm>
#include <atomic>
#include <memory>

#include "engine.h"
#include "modules.h"
#include "stages.h"

namespace mod {

class SynthPatch {
 public:
  enum ModuleId { kClock = 0, kSeq = 1, kOsc = 2, kStages = 3, kVca = 4, kModuleCount = 5 };

  // Monta e compila o patch. Chamar fora da thread de áudio, com o áudio parado.
  bool build(float sampleRate, int block = 96) {
    release();
    eng_ = std::make_unique<Engine>(sampleRate, block);
    auto* clock = eng_->add<Clock>();
    auto* seq = eng_->add<Seq4>();
    auto* osc = eng_->add<SineOsc>();
    auto* st = eng_->add<Stages>();
    auto* vca = eng_->add<Vca>();
    auto* out = eng_->add<AudioOut>();
    mods_[kClock] = clock; mods_[kSeq] = seq; mods_[kOsc] = osc; mods_[kStages] = st; mods_[kVca] = vca;

    clock->setParam(0, 3.f);    // 3 notas por segundo
    clock->setParam(1, 0.6f);   // gate alto em 60% do período
    const float notes[4] = {0.f, 7.f, 12.f, 3.f};
    for (int i = 0; i < 4; ++i) seq->setParam(i, notes[i]);

    // Stages como ADSR: [ramp][ramp][hold em loop][ramp]; o gate do clock entra no segmento 1 e um
    // gate fantasma no segmento 5 separa os segmentos 5 e 6 do grupo.
    const int types[4] = {Stages::kRamp, Stages::kRamp, Stages::kHold, Stages::kRamp};
    const float sliders[4] = {Stages::timeToSlider(0.01f), Stages::timeToSlider(0.12f), 0.5f, Stages::timeToSlider(0.3f)};
    for (int i = 0; i < 4; ++i) {
      st->setParam(Stages::kType + i, static_cast<float>(types[i]));
      st->setParam(Stages::kSlider + i, sliders[i]);
    }
    st->setParam(Stages::kLoopStart + 2, 1.f);
    st->setParam(Stages::kLoopEnd + 2, 1.f);
    st->setParam(Stages::kDummyGate + 4, 1.f);

    eng_->connect(clock, 0, seq, 0);
    eng_->connect(clock, 0, st, 0);
    eng_->connect(seq, 0, osc, 0);
    eng_->connect(osc, 0, vca, 0);
    eng_->connect(st, 0, vca, 1);
    eng_->connect(vca, 0, out, 0);
    eng_->connect(vca, 0, out, 1);
    if (!eng_->compile(out)) { release(); return false; }
    return true;
  }

  bool ready() const { return eng_ != nullptr; }

  void release() {
    eng_.reset();
    for (auto& m : mods_) m = nullptr;
  }

  // Thread de áudio.
  void render(float* left, float* right, int frames) {
    eng_->render(left, right, frames);
    const float g = master_.load(std::memory_order_relaxed);
    for (int i = 0; i < frames; ++i) { left[i] *= g; right[i] *= g; }
  }

  // Qualquer thread (parâmetros são atômicos). Índices inválidos são ignorados.
  void setMaster(float gain) { master_.store(std::min(1.f, std::max(0.f, gain)), std::memory_order_relaxed); }
  void setParam(int module, int index, float value) {
    if (module < 0 || module >= kModuleCount || !mods_[module]) return;
    if (index < 0 || index >= mods_[module]->numParams()) return;
    mods_[module]->setParam(index, value);
  }
  int numLeds(int module) const {
    return (module < 0 || module >= kModuleCount || !mods_[module]) ? 0 : mods_[module]->numLeds();
  }
  bool getLed(int module, int index, float* rgb) const {
    if (index < 0 || index >= numLeds(module)) return false;
    mods_[module]->getLed(index, rgb);
    return true;
  }

 private:
  std::unique_ptr<Engine> eng_;
  Module* mods_[kModuleCount] = {};
  std::atomic<float> master_{0.4f};
};

}  // namespace mod
