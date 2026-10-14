// Ponte entre o motor modular (synth_patch.h) e o Android: stream de áudio com Oboe + funções JNI.
//
// Regras de tempo real: o callback de áudio nunca trava (sem mutex, sem alocação); parâmetros chegam
// por atômicos dentro dos módulos. O patch só é construído/destruído com o stream parado.

#include <android/log.h>
#include <jni.h>
#include <oboe/Oboe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

#include "synth_patch.h"

#define LOG_TAG "SynthModular"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kChunk = 512;  // o callback do Android pode pedir mais frames; processamos em pedaços

class AudioApp {
 public:
  AudioApp() : callback_(std::make_shared<Callback>(this)) {
    left_.assign(kChunk, 0.f);
    right_.assign(kChunk, 0.f);
  }

  bool start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_) return true;
    disconnected_.store(false);

    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSharingMode(oboe::SharingMode::Exclusive);  // se não der, o Oboe abre em modo compartilhado
    builder.setFormat(oboe::AudioFormat::Float);
    builder.setChannelCount(oboe::ChannelCount::Stereo);
    builder.setUsage(oboe::Usage::Media);
    builder.setFormatConversionAllowed(true);
    builder.setChannelConversionAllowed(true);
    builder.setDataCallback(callback_);
    builder.setErrorCallback(callback_);

    oboe::Result r = builder.openStream(stream_);
    if (r != oboe::Result::OK) {
      LOGE("openStream falhou: %s", oboe::convertToText(r));
      stream_.reset();
      return false;
    }
    // Buffer de 2 bursts: bom equilíbrio entre latência e segurança contra falhas de áudio.
    stream_->setBufferSizeInFrames(stream_->getFramesPerBurst() * 2);

    sampleRate_ = static_cast<double>(stream_->getSampleRate());
    if (!patch_.build(static_cast<float>(sampleRate_))) {
      LOGE("patch.build falhou");
      stream_->close();
      stream_.reset();
      return false;
    }
    r = stream_->requestStart();
    if (r != oboe::Result::OK) {
      LOGE("requestStart falhou: %s", oboe::convertToText(r));
      stream_->close();
      stream_.reset();
      patch_.release();
      return false;
    }
    LOGI("áudio ligado: %.0f Hz, burst %d, buffer %d", sampleRate_, stream_->getFramesPerBurst(),
         stream_->getBufferSizeInFrames());
    return true;
  }

  void stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_) {
      stream_->requestStop();
      stream_->close();  // depois do close o callback não roda mais
      stream_.reset();
    }
    patch_.release();
  }

  void setParam(int module, int index, float value) {
    std::lock_guard<std::mutex> lock(mutex_);
    patch_.setParam(module, index, value);
  }
  void setMaster(float gain) { patch_.setMaster(gain); }  // atômico

  bool getLed(int module, int index, float* rgb) {
    std::lock_guard<std::mutex> lock(mutex_);
    return patch_.getLed(module, index, rgb);
  }

  // out[0] taxa, [1] burst, [2] buffer, [3] xruns, [4] carga (0..1), [5] latência em ms (-1 = desconhecida),
  // [6] desconectado (0/1), [7] exclusivo (0/1), [8] API (0 não especificada, 1 OpenSL ES, 2 AAudio)
  void stats(float* out, int n) {
    std::lock_guard<std::mutex> lock(mutex_);
    float v[9] = {0, 0, 0, 0, 0, -1, 0, 0, 0};
    v[6] = disconnected_.load() ? 1.f : 0.f;
    if (stream_) {
      v[0] = static_cast<float>(stream_->getSampleRate());
      v[1] = static_cast<float>(stream_->getFramesPerBurst());
      v[2] = static_cast<float>(stream_->getBufferSizeInFrames());
      auto x = stream_->getXRunCount();
      v[3] = x.error() == oboe::Result::OK ? static_cast<float>(x.value()) : -1.f;
      v[4] = load_.load();
      auto lat = stream_->calculateLatencyMillis();
      v[5] = lat.error() == oboe::Result::OK ? static_cast<float>(lat.value()) : -1.f;
      v[7] = stream_->getSharingMode() == oboe::SharingMode::Exclusive ? 1.f : 0.f;
      v[8] = stream_->getAudioApi() == oboe::AudioApi::AAudio ? 2.f
             : (stream_->getAudioApi() == oboe::AudioApi::OpenSLES ? 1.f : 0.f);
    }
    for (int i = 0; i < n && i < 9; ++i) out[i] = v[i];
  }

 private:
  class Callback : public oboe::AudioStreamDataCallback, public oboe::AudioStreamErrorCallback {
   public:
    explicit Callback(AudioApp* app) : app_(app) {}
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream*, void* audioData, int32_t numFrames) override {
      return app_->render(static_cast<float*>(audioData), numFrames);
    }
    void onErrorAfterClose(oboe::AudioStream*, oboe::Result error) override {
      LOGE("stream encerrado por erro: %s", oboe::convertToText(error));
      app_->disconnected_.store(true);  // a interface percebe e oferece religar
    }

   private:
    AudioApp* app_;
  };

  // Thread de áudio.
  oboe::DataCallbackResult render(float* out, int32_t numFrames) {
    const auto t0 = std::chrono::steady_clock::now();
    int done = 0;
    while (done < numFrames) {
      const int c = std::min<int>(numFrames - done, kChunk);
      patch_.render(left_.data(), right_.data(), c);
      for (int i = 0; i < c; ++i) {
        out[2 * (done + i)] = left_[i];
        out[2 * (done + i) + 1] = right_[i];
      }
      done += c;
    }
    const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double budget = static_cast<double>(numFrames) / sampleRate_;
    load_.store(0.95f * load_.load() + 0.05f * static_cast<float>(dt / budget));  // média suavizada
    return oboe::DataCallbackResult::Continue;
  }

  std::mutex mutex_;  // protege start/stop/params/stats; o callback de áudio NÃO usa
  std::shared_ptr<Callback> callback_;
  std::shared_ptr<oboe::AudioStream> stream_;
  mod::SynthPatch patch_;
  std::vector<float> left_, right_;
  double sampleRate_ = 48000.0;
  std::atomic<float> load_{0.f};
  std::atomic<bool> disconnected_{false};
};

AudioApp& app() {
  static AudioApp instance;
  return instance;
}

}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL Java_com_example_synthmodular_NativeAudio_nativeStart(JNIEnv*, jclass) {
  return app().start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_example_synthmodular_NativeAudio_nativeStop(JNIEnv*, jclass) { app().stop(); }

JNIEXPORT void JNICALL Java_com_example_synthmodular_NativeAudio_nativeSetParam(JNIEnv*, jclass, jint module,
                                                                                 jint index, jfloat value) {
  app().setParam(module, index, value);
}

JNIEXPORT void JNICALL Java_com_example_synthmodular_NativeAudio_nativeSetMaster(JNIEnv*, jclass, jfloat gain) {
  app().setMaster(gain);
}

JNIEXPORT jint JNICALL Java_com_example_synthmodular_NativeAudio_nativeLed(JNIEnv* env, jclass, jint module,
                                                                            jint index, jfloatArray out) {
  float rgb[3] = {0.f, 0.f, 0.f};
  const bool ok = app().getLed(module, index, rgb);
  env->SetFloatArrayRegion(out, 0, 3, rgb);
  return ok ? 1 : 0;
}

JNIEXPORT void JNICALL Java_com_example_synthmodular_NativeAudio_nativeStats(JNIEnv* env, jclass, jfloatArray out) {
  float v[9];
  app().stats(v, 9);
  const jsize n = std::min<jsize>(env->GetArrayLength(out), 9);
  env->SetFloatArrayRegion(out, 0, n, v);
}

}  // extern "C"
