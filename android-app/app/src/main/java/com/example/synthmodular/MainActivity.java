package com.example.synthmodular;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Tela de teste: liga o áudio, mexe nos parâmetros do patch (clock, envelope do Stages) e mostra os LEDs do
 * Stages e as medidas de desempenho (latência, carga, xruns) para avaliar o Galaxy S10.
 */
public class MainActivity extends Activity {

    /** Aplica o valor e devolve o texto para mostrar ao lado do nome. */
    private interface OnValue {
        String set(int progress);
    }

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final View[] leds = new View[12];
    private final float[] rgb = new float[3];
    private final float[] stats = new float[9];
    private final List<Runnable> appliers = new ArrayList<>();
    private TextView statsView;
    private Button toggle;
    private boolean running = false;

    private final Runnable tick = new Runnable() {
        @Override
        public void run() {
            if (!running) return;
            for (int i = 0; i < 12; i++) {
                NativeAudio.nativeLed(NativeAudio.STAGES, i, rgb);
                leds[i].setBackgroundColor(Color.rgb(shade(rgb[0]), shade(rgb[1]), shade(rgb[2])));
            }
            NativeAudio.nativeStats(stats);
            if (stats[6] > 0.5f) {
                stopAudio();
                statsView.setText("O áudio foi desconectado (fones removidos ou rota trocada). Toque em Ligar áudio.");
                return;
            }
            statsView.setText(String.format(Locale.US,
                    "Taxa: %.0f Hz\nBurst: %.0f frames\nBuffer: %.0f frames\nXruns: %.0f\n"
                            + "CPU do áudio: %.1f%%\nLatência: %s\nAPI: %s, modo %s",
                    stats[0], stats[1], stats[2], stats[3], stats[4] * 100f,
                    stats[5] < 0 ? "indisponível" : String.format(Locale.US, "%.1f ms", stats[5]),
                    stats[8] == 2f ? "AAudio" : (stats[8] == 1f ? "OpenSL ES" : "?"),
                    stats[7] > 0.5f ? "exclusivo" : "compartilhado"));
            handler.postDelayed(this, 50);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(16);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);
        setContentView(scroll);

        TextView title = new TextView(this);
        title.setText("Synth modular: teste de áudio");
        title.setTextSize(20f);
        root.addView(title);

        toggle = new Button(this);
        toggle.setText("Ligar áudio");
        toggle.setOnClickListener(v -> toggleAudio());
        root.addView(toggle);

        addSeek(root, "Volume", 50, p -> {
            NativeAudio.nativeSetMaster(p / 100f * 0.8f);
            return p + "%";
        });
        addSeek(root, "Andamento", 33, p -> {
            float hz = 0.5f + p / 100f * 7.5f;
            NativeAudio.nativeSetParam(NativeAudio.CLOCK, 0, hz);
            return String.format(Locale.US, "%.1f notas/s", hz);
        });
        addSeek(root, "Duração da nota", 59, p -> {
            float w = 0.1f + p / 100f * 0.85f;
            NativeAudio.nativeSetParam(NativeAudio.CLOCK, 1, w);
            return Math.round(w * 100) + "% do período";
        });
        addSeek(root, "Ataque", 30, p -> time(0, p));
        addSeek(root, "Decaimento", 63, p -> time(1, p));
        addSeek(root, "Sustentação", 50, p -> {
            NativeAudio.nativeSetParam(NativeAudio.STAGES, NativeAudio.ST_SLIDER + 2, p / 100f);
            return Math.round(p / 100f * 8f * 10f) / 10f + " V";
        });
        addSeek(root, "Release", 75, p -> time(3, p));
        addSeek(root, "Curva do ataque", 50, p -> {
            NativeAudio.nativeSetParam(NativeAudio.STAGES, NativeAudio.ST_POT, p / 100f);
            return p < 45 ? "acelerada" : (p > 55 ? "desacelerada" : "linear");
        });

        TextView ledTitle = new TextView(this);
        ledTitle.setText("\nLEDs do Stages (cima: tipo de cada segmento; baixo: nível das saídas)");
        root.addView(ledTitle);
        root.addView(ledRow(0));
        root.addView(ledRow(6));

        statsView = new TextView(this);
        statsView.setPadding(0, dp(16), 0, 0);
        statsView.setText("Áudio desligado.");
        root.addView(statsView);
    }

    @Override
    protected void onStop() {
        super.onStop();
        if (running) stopAudio();
    }

    private void toggleAudio() {
        if (running) {
            stopAudio();
            return;
        }
        if (!NativeAudio.nativeStart()) {
            statsView.setText("Não foi possível abrir o áudio. Veja o Logcat (filtro SynthModular).");
            return;
        }
        running = true;
        toggle.setText("Parar áudio");
        for (Runnable r : appliers) r.run();  // sincroniza o motor com a posição dos controles
        handler.post(tick);
    }

    private void stopAudio() {
        running = false;
        handler.removeCallbacks(tick);
        NativeAudio.nativeStop();
        toggle.setText("Ligar áudio");
    }

    /** Tempo de um segmento do Stages: 1 ms a 2 s em escala exponencial. */
    private String time(int segment, int p) {
        float seconds = (float) (0.001 * Math.pow(2000.0, p / 100.0));
        NativeAudio.nativeSetParam(NativeAudio.STAGES, NativeAudio.ST_SLIDER + segment,
                NativeAudio.timeToSlider(seconds));
        return seconds < 1f ? Math.round(seconds * 1000f) + " ms"
                : String.format(Locale.US, "%.2f s", seconds);
    }

    private void addSeek(LinearLayout parent, String name, int initial, OnValue onValue) {
        TextView label = new TextView(this);
        SeekBar bar = new SeekBar(this);
        bar.setMax(100);
        bar.setProgress(initial);
        Runnable apply = () -> label.setText(name + ": " + onValue.set(bar.getProgress()));
        appliers.add(apply);
        label.setText(name);
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                if (fromUser) apply.run();
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {}

            @Override
            public void onStopTrackingTouch(SeekBar s) {}
        });
        parent.addView(label);
        parent.addView(bar);
    }

    private LinearLayout ledRow(int first) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < 6; i++) {
            View v = new View(this);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(36), dp(36));
            lp.setMargins(dp(4), dp(4), dp(4), dp(4));
            v.setLayoutParams(lp);
            v.setBackgroundColor(Color.rgb(40, 40, 40));
            leds[first + i] = v;
            row.addView(v);
        }
        return row;
    }

    /** LED apagado = cinza escuro; aceso = cor do LED. */
    private static int shade(float c) {
        return 40 + Math.round(215f * Math.max(0f, Math.min(1f, c)));
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
