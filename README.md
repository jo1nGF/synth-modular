# Synth modular (esqueleto do motor)

Motor modular em C++17, só com headers, feito para rodar igual no PC e no Android (NDK).
A ideia é validar o motor no PC e só depois levar para o celular.

## O que tem aqui

| Arquivo | Para quê |
|---|---|
| `src/engine.h` | Módulo base, cabos, ordenação topológica, renderizador em blocos fixos |
| `src/modules.h` | Módulos de teste: Clock, Seq4, SineOsc, AdEnv, Vca |
| `src/stages.h` | Stages com os modos do firmware do qiemem, implementação própria (não é um port) |
| `src/synth_patch.h` | Patch de demonstração do app Android (sem dependências de Android) |
| `tests/test_patch.cpp` | 11 verificações do patch do app |
| `android-app/` | Projeto Android (Gradle + CMake + Oboe + JNI) para tocar o motor ao vivo; ver `android-app/README-android.md` |
| `tests/test_stages.cpp` | 147 verificações: exemplos do manual (ADSR, AD, sequência, S&H, pulso, LFO...) e recursos do firmware do qiemem |
| `src/main_desktop.cpp` | Monta um patch, grava `demo.wav` e mede o custo de CPU |
| `adapters/plaits_module.h` | Adaptador do Plaits, escrito conforme a API do código original (ver "Verificado") |
| `CMakeLists.txt` | Build do PC e alvo `modular_core` para reaproveitar no Android |

## Rodar no PC

```
g++ -std=c++17 -O2 -Isrc src/main_desktop.cpp -o demo
./demo demo.wav
```

Ou com CMake: `cmake -S . -B build && cmake --build build && ./build/demo`.
**Nota:** o CMake não foi testado (o ambiente em que escrevi não tinha cmake). Os comandos `g++` acima foram testados.

Resultado esperado: 16 notas (4 por segundo, 4 s), a primeira em C4 (261,6 Hz).
Esse teste foi rodado e conferido. Em um PC, 5 módulos simples renderizam centenas de vezes
mais rápido que o tempo real. Esse número **não** vale para o S10: mede-se de novo no celular.

## Convenções de sinal

- Áudio: -1.0 a +1.0
- V/oct: 1.0 = 1 oitava; 0.0 = C4
- Gate: 0.0 ou 1.0
- Envelopes e CV unipolar: 0.0 a 1.0
- Tudo mono, um `float` por frame, na taxa do motor (48 kHz sugerido)

## Regras de tempo real

- Nada é alocado dentro de `process()`; tudo é alocado em `Engine::compile()`.
- Parâmetros são atômicos: a UI pode mudá-los de outra thread.
- Mudar o grafo (adicionar módulo ou cabo) deve ser feito fora da thread de áudio.
  Falta implementar a troca segura do grafo (compilar um novo e trocar o ponteiro).
- Os módulos sempre recebem blocos de exatamente `block` frames (96 no demo).
  O `render()` aceita qualquer tamanho de callback e cuida disso por dentro.

## O que foi verificado (leitura do código público)

Eu li o repositório pela web, sem clonar. As páginas de árvore de pastas do GitHub bloqueiam
acesso automático, então a leitura veio de arquivos individuais, de um UGen público que usa o
Plaits e de commits do repositório oficial.

**Confirmado:**
- O repositório oficial tem uma pasta por módulo, incluindo `stages/`, e o `stmlib` entra como submódulo.
- Licença: projetos STM32 sob MIT, projetos AVR sob GPL 3.0.
- API do Plaits (firmware 1.2): `plaits/dsp/voice.h`, `Voice::Init(BufferAllocator*)` com buffer de
  16384 bytes, `Voice::Render(patch, modulations, frames, kBlockSize)`, saída `out`/`aux` em int16,
  `patch.note = 60 + 12 * V/oct`. O adaptador segue exatamente isso.
- O Plaits roda a 48 kHz. Em outra taxa o som muda de forma audível.
- Stages: são 6 `SegmentGenerator` (um por canal). `Process(gate_flags, out, size)` produz `out[i].value`.
  A configuração dos grupos de segmentos vem de `chain_state` (conforme quais entradas de gate
  estão ligadas), e os 3 tipos de segmento são ramp, step e hold.

**Ainda não confirmado:**
- Os headers reais do Plaits e do Stages não foram compilados. O adaptador foi testado só contra
  um stub com a mesma forma da API (compila sem avisos e toca as 4 notas na afinação certa).
- Stages: não consegui abrir `segment_generator.h`, então faltam as assinaturas de `Configure()` e
  dos parâmetros por segmento. Atenção: forks (como o qiemem) mudam a API. No commit oficial de 2020
  `SegmentGenerator::Init()` não recebe argumentos, e no fork recebe `&settings`.

## Stages (implementado, com os modos do firmware do qiemem)

Implementação própria (`src/stages.h`), escrita a partir do manual oficial e do README do firmware
alternativo do qiemem (Bryan Head, github.com/qiemem/eurorack). **Não é um port do código.**
Rodar os testes: `g++ -std=c++17 -O2 -Isrc tests/test_stages.cpp -o test_stages && ./test_stages`.

**Modos** (parâmetro `kMode`, na ordem do firmware do qiemem)
- **0 Gerador de segmentos:** o comportamento original, mais as melhorias do qiemem (polaridade bipolar,
  re-trigger desligável nas rampas, faixas de tempo e de frequência de LFO, LFO que reseta no trigger,
  quantização de step e hold em cromática/maior/pentatônica, razões de clock lentas/médias/rápidas) e as
  do firmware oficial 1.2 (a rampa acompanha o nível do segmento anterior, probabilidade de disparo no
  gate looping vermelho).
- **1 Avançado:** segmento aleatório (caos de Chen, registrador de deslocamento tipo Turing Machine,
  LFO aleatório livre e sincronizado), slew com subida e descida independentes (com retificação no modo
  unipolar) e atenuverter (step em loop).
- **2 Seis DAHDSR independentes** e **3 seis DAHDSR idênticos.**
- **4 e 5 Oscilador harmônico (Ouroboros) e a variante com controles alternativos.** Coluna 0 = raiz
  (slider = afinação grossa, pot = fina, CV = V/oct na convenção do motor, saída = mix); colunas 1 a 5 =
  parciais (razão pelo pot e volume por slider+CV, ou o contrário no modo 5; o gate faz o strum).
  Seis formas de onda (seno, triângulo, quadrada, serra e dois pulsos, com bandlimiting PolyBLEP),
  as três faixas de frequência da raiz (alta de áudio e duas de LFO) e as três faixas de razões harmônicas.
  Em faixa de LFO, o gate reseta a fase e um cabo no gate tira o parcial do mix.

**Aproximações minhas** (marcadas `[ASSUMIDO]` no código; o README do qiemem não dá a fórmula):
mapeamento do tempo de hold e do tempo base (exponencial), formato das curvas, glide do step, formas
do LFO por região do pot, escala de tempo e de saída do atrator caótico, curva dos tempos do DAHDSR
(durações de 0 a 10 s, curva cúbica), lógica do segmento aleatório dentro de grupos (simplificada) e, no
Ouroboros, a afinação fina (±1 semitom), o decaimento do strum (~120 ms), as larguras dos pulsos, a
zona de snap entre razões e a normalização do mix.

**Não implementado:** oscilador de áudio de segmento isolado do firmware oficial 1.2 (já especificado na base de conhecimento),
encadeamento de módulos, osciladores de LFO em taxa de áudio com banda limitada (no modo 0 e 1), gestos de
botão (aqui tudo é parâmetro) e o atraso de retrigger do firmware.

**Testes:** 147 verificações. Também quebrei regras de propósito (mutação) em 25 pontos e os testes
falharam nos casos esperados.

## Sequenciador estendido (firmware oficial 1.2)

Um HOLD ou RAMP no início de uma sequência de STEPs (por exemplo RAMP + 4 STEPs). O primeiro segmento não
é um passo: o pot dele escolhe o modo (para cima, para baixo, pêndulo, alternado 1-2-1-3-1-4..., aleatório,
pseudoaleatório sem repetição, endereçável), o gate dele é o clock, e slider + CV viram o endereço (modo
endereçável) ou a entrada de reset (nos demais). Reset: volta ao primeiro passo e inibe o clock por 5 ms; o clock
também fica inibido com o reset alto. Primeiro HOLD: sem quantização (0 a 8 V). Primeiro RAMP: 0 a 1 V em
semitons. Um loop restringe a reprodução aos passos em loop. As saídas 2 a N mostram o passo ativo.
**Aproximações minhas:** a detecção (primeiro HOLD/RAMP + ao menos 2 STEPs), as zonas iguais do pot para os 7
modos, o limiar de reset de 4 V e o primeiro clock tocar o passo 1 (o manual não diz).

## LEDs do painel

Qualquer módulo pode expor LEDs: `numLeds()` e `getLed(i, rgb)` (a UI lê a qualquer momento, sem travar o
áudio). O Stages tem **12 LEDs**, como na foto do painel: 6 de tipo (acima dos botões, índices 0 a 5) e 6 ao lado
das saídas (índices 6 a 11, que mostram o nível da saída; essa função é suposição minha). O Plaits tem 8 LEDs
de modelo: acende o LED `engine % 8` e a cor mostra o banco (verde, vermelho e laranja).

- **Cor:** verde = ramp, laranja = step, vermelho = hold, aleatório = oscila suave entre as três.
- **Brilho:** acompanha o sinal de saída do segmento (piso de 0,2 em repouso).
- **Loop:** o LED pisca (ramp: suave; step e hold: seco, como no modo daltônico oficial). Num loop de vários
  segmentos, o primeiro e o último piscam em "ping pong". Em LFO a taxa do piscar depende da faixa.
- **Bipolar ou sem re-trigger:** piscada vermelha fraca 1 vez por segundo (README do qiemem).
- **Modo daltônico** (parâmetro `kColorBlind`, firmware 1.1): ramp com brilho moderado e leve modulação,
  step brilhante, hold fraco.
- **DAHDSR:** verde em delay/attack/hold/decay, laranja no sustain, vermelho no release, apagado em repouso
  (no modo independente o selecionado fica amarelo em repouso).
- **Ouroboros:** a cor mostra a forma de onda (seno verde, triângulo laranja, quadrada vermelha; serra e
  pulsos piscando).

**Aproximações minhas:** as taxas de piscar, o piso de brilho e a curva brilho x sinal (o manual só mostra
isso em imagens). **Não implementado:** o LED do slider que pisca mais devagar ao se aproximar de um dó no
oscilador de áudio, e a indicação de estágio desligado nos DAHDSR.

## Ligar o Plaits

1. Pegue `plaits/` e `stmlib/` do repositório oficial (pichenettes/eurorack) quando tiver acesso a um PC.
2. Compile os `.cc` de `plaits/dsp/`, `plaits/resources.cc` e os do `stmlib/` junto com o demo,
   com `-DWITH_MI_PLAITS -I<eurorack> -Iadapters`.
3. No `main_desktop.cpp`, troque o `SineOsc` por `PlaitsModule`.

O motor precisa de `block` múltiplo de 12 (o demo usa 96). Se não for, o módulo fica mudo em vez de
tocar áudio quebrado.

## Android

O projeto em `android-app/` já faz isso (stream Oboe de baixa latência + JNI + tela de teste); falta compilar e
medir no S10. O texto abaixo é o plano original, mantido como referência:


Plano: app Kotlin com a UI e uma biblioteca nativa (CMake + NDK, `arm64-v8a`) que usa `modular_core`.
Áudio com Oboe, em modo de baixa latência:

```cpp
oboe::AudioStreamBuilder b;
b.setDirection(oboe::Direction::Output)
 ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
 ->setSharingMode(oboe::SharingMode::Exclusive)
 ->setFormat(oboe::AudioFormat::Float)
 ->setChannelCount(2)
 ->setSampleRate(48000)
 ->setDataCallback(&callback)
 ->openStream(stream);

// No callback: chamar engine.render(l, r, numFrames) e intercalar L/R na saída.
// Se numFrames for maior que seus buffers L/R, chame render() em pedaços.
```

## Próximos passos

1. Conferir as aproximações do Stages contra o código oficial, se for possível lê-lo.
2. Compilar o Plaits real e conferir o adaptador (precisa de PC, ou de um ambiente que clone o repositório).
3. Medir o custo de CPU por módulo no S10 e definir um orçamento por bloco.
4. UI de patch por toque e troca segura do grafo.
