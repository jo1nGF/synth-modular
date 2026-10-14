package com.example.synthmodular;

/** Ponte para a biblioteca nativa (native-lib.cpp). Os IDs e índices abaixo espelham synth_patch.h e stages.h. */
public final class NativeAudio {
    static {
        System.loadLibrary("synthmodular");
    }

    private NativeAudio() {}

    public static native boolean nativeStart();
    public static native void nativeStop();
    public static native void nativeSetParam(int module, int index, float value);
    public static native void nativeSetMaster(float gain);
    /** Preenche rgb[0..2] com a cor do LED (0 a 1). Retorna 1 se o LED existe. */
    public static native int nativeLed(int module, int index, float[] rgb);
    /** Preenche até 9 números: taxa, burst, buffer, xruns, carga, latência ms, desconectado, exclusivo, API. */
    public static native void nativeStats(float[] out);

    // IDs de módulos (synth_patch.h)
    public static final int CLOCK = 0, SEQ = 1, OSC = 2, STAGES = 3, VCA = 4;
    // Índices de parâmetros do Stages (stages.h)
    public static final int ST_SLIDER = 6, ST_POT = 12;

    /** Mesmo mapeamento de Stages::timeToSlider: de tempo em segundos para a posição do slider (1 ms a 16 s). */
    public static float timeToSlider(float seconds) {
        return (float) (Math.log(Math.max(seconds, 0.001f) / 0.001f) / Math.log(16000.0));
    }
}
