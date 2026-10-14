#pragma once
// Motor modular minimalista para Android/desktop.
//
// Convenções de sinal (todas mono, float, uma amostra por frame):
//   áudio  : -1.0 .. +1.0
//   V/oct  : 1.0 = 1 oitava, 0.0 = C4 (261.6256 Hz)
//   gate   : 0.0 ou 1.0
//   envelope/CV unipolar : 0.0 .. 1.0
//
// Regras de tempo real:
//   - Nada é alocado em process(). Tudo é alocado em Engine::compile().
//   - Parâmetros são atômicos, então a UI pode alterá-los de outra thread.
//   - Mudar o grafo (add/connect) só deve ser feito fora da thread de áudio.
//   - Os módulos sempre são chamados com exatamente `block` frames.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

namespace mod {

constexpr float kPi = 3.14159265358979f;

class Module {
 public:
  Module(int numIn, int numOut, int numParams = 8, int numLeds = 0)
      : in_(numIn, nullptr),
        connected_(numIn, 0),
        out_(numOut),
        params_(new std::atomic<float>[numParams]),
        numParams_(numParams),
        leds_(new std::atomic<float>[numLeds > 0 ? numLeds * 3 : 1]),
        numLeds_(numLeds) {
    for (int i = 0; i < numParams; ++i) params_[i].store(0.f, std::memory_order_relaxed);
    for (int i = 0; i < numLeds * 3; ++i) leds_[i].store(0.f, std::memory_order_relaxed);
  }
  virtual ~Module() = default;

  // Chamado por Engine::compile(), fora da thread de áudio.
  // Quem sobrescrever DEVE chamar Module::prepare(...) primeiro.
  virtual void prepare(float sampleRate, int block) {
    sampleRate_ = sampleRate;
    block_ = block;
    for (auto& b : out_) b.assign(block, 0.f);
  }

  // Chamado depois que todos os cabos foram ligados (útil para módulos que
  // mudam de comportamento conforme as entradas conectadas, como o Stages).
  virtual void onCompiled() {}

  // Sempre chamado com frames == block.
  virtual void process(int frames) = 0;

  void setParam(int i, float v) { params_[i].store(v, std::memory_order_relaxed); }
  float param(int i) const { return params_[i].load(std::memory_order_relaxed); }

  // LEDs do painel: o módulo atualiza (thread de áudio) e a UI lê a qualquer momento.
  // Cada LED é RGB com componentes de 0 a 1 (o brilho já vem embutido na cor).
  int numParams() const { return numParams_; }
  int numLeds() const { return numLeds_; }
  void getLed(int i, float* rgb) const {
    for (int c = 0; c < 3; ++c) rgb[c] = leds_[i * 3 + c].load(std::memory_order_relaxed);
  }

  int numIn() const { return static_cast<int>(in_.size()); }
  int numOut() const { return static_cast<int>(out_.size()); }
  const float* output(int p) const { return out_[p].data(); }
  void setInput(int p, const float* buf, bool connected = false) {
    in_[p] = buf;
    connected_[p] = connected ? 1 : 0;
  }
  bool inputConnected(int p) const { return connected_[p] != 0; }

 protected:
  void setLed(int i, float r, float g, float b) {
    leds_[i * 3 + 0].store(r, std::memory_order_relaxed);
    leds_[i * 3 + 1].store(g, std::memory_order_relaxed);
    leds_[i * 3 + 2].store(b, std::memory_order_relaxed);
  }
  const float* in(int p) const { return in_[p]; }
  float* out(int p) { return out_[p].data(); }

  float sampleRate_ = 48000.f;
  int block_ = 0;

 private:
  std::vector<const float*> in_;
  std::vector<char> connected_;
  std::vector<std::vector<float>> out_;
  std::unique_ptr<std::atomic<float>[]> params_;
  int numParams_;
  std::unique_ptr<std::atomic<float>[]> leds_;
  int numLeds_;
};

// Saída estéreo final. Ligue as entradas 0 (L) e 1 (R).
class AudioOut : public Module {
 public:
  AudioOut() : Module(2, 0) {}
  void process(int) override {}
  const float* left() const { return in(0); }
  const float* right() const { return in(1); }
};

class Engine {
 public:
  Engine(float sampleRate, int block)
      : sr_(sampleRate), block_(block), zeros_(block, 0.f), lbuf_(block, 0.f), rbuf_(block, 0.f) {}

  template <class T, class... Args>
  T* add(Args&&... args) {
    auto m = std::make_unique<T>(std::forward<Args>(args)...);
    T* raw = m.get();
    modules_.push_back(std::move(m));
    return raw;
  }

  void connect(Module* src, int srcPort, Module* dst, int dstPort) {
    cables_.push_back({src, srcPort, dst, dstPort});
  }

  // Ordena os módulos (ordenação topológica), aloca buffers e liga os cabos.
  // Retorna false se houver cabo inválido ou ciclo (feedback sem atraso).
  bool compile(AudioOut* audioOut) {
    audioOut_ = audioOut;
    const int n = static_cast<int>(modules_.size());

    auto indexOf = [&](Module* m) {
      for (int i = 0; i < n; ++i)
        if (modules_[i].get() == m) return i;
      return -1;
    };

    std::vector<std::vector<int>> adj(n);
    std::vector<int> indeg(n, 0);
    for (const auto& c : cables_) {
      const int a = indexOf(c.src), b = indexOf(c.dst);
      if (a < 0 || b < 0) return false;
      if (c.srcPort < 0 || c.srcPort >= c.src->numOut()) return false;
      if (c.dstPort < 0 || c.dstPort >= c.dst->numIn()) return false;
      adj[a].push_back(b);
      indeg[b]++;
    }

    std::vector<int> queue;
    for (int i = 0; i < n; ++i)
      if (indeg[i] == 0) queue.push_back(i);
    order_.clear();
    for (size_t h = 0; h < queue.size(); ++h) {
      const int u = queue[h];
      order_.push_back(modules_[u].get());
      for (int v : adj[u])
        if (--indeg[v] == 0) queue.push_back(v);
    }
    if (static_cast<int>(order_.size()) != n) return false;  // ciclo

    // 1) aloca buffers e zera entradas; 2) liga os cabos.
    for (auto& m : modules_) {
      m->prepare(sr_, block_);
      for (int p = 0; p < m->numIn(); ++p) m->setInput(p, zeros_.data());
    }
    for (const auto& c : cables_) c.dst->setInput(c.dstPort, c.src->output(c.srcPort), true);
    for (auto& m : modules_) m->onCompiled();
    return true;
  }

  // Gera exatamente `frames` frames estéreo. Pode ser chamado de um callback
  // de áudio com qualquer número de frames; internamente os módulos sempre
  // rodam em blocos fixos de `block` frames.
  void render(float* left, float* right, int frames) {
    int done = 0;
    while (done < frames) {
      if (avail_ == 0) {
        for (Module* m : order_) m->process(block_);
        if (audioOut_) {
          std::memcpy(lbuf_.data(), audioOut_->left(), sizeof(float) * block_);
          std::memcpy(rbuf_.data(), audioOut_->right(), sizeof(float) * block_);
        }
        pos_ = 0;
        avail_ = block_;
      }
      const int take = std::min(avail_, frames - done);
      std::memcpy(left + done, lbuf_.data() + pos_, sizeof(float) * take);
      std::memcpy(right + done, rbuf_.data() + pos_, sizeof(float) * take);
      pos_ += take;
      avail_ -= take;
      done += take;
    }
  }

  float sampleRate() const { return sr_; }
  int block() const { return block_; }

 private:
  struct Cable {
    Module* src;
    int srcPort;
    Module* dst;
    int dstPort;
  };

  float sr_;
  int block_;
  std::vector<float> zeros_, lbuf_, rbuf_;
  std::vector<std::unique_ptr<Module>> modules_;
  std::vector<Cable> cables_;
  std::vector<Module*> order_;
  AudioOut* audioOut_ = nullptr;
  int pos_ = 0, avail_ = 0;
};

}  // namespace mod
