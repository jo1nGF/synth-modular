# App Android: teste de áudio ao vivo

Projeto mínimo (Gradle + CMake + Oboe + JNI) que toca o motor modular no celular e mostra o desempenho.
O patch é fixo: clock → sequenciador de 4 notas → oscilador senoidal → VCA, com um **Stages** como
envelope ADSR. A tela tem sliders (volume, andamento, envelope), os 12 LEDs do Stages e as medidas do áudio
(taxa, burst, buffer, xruns, carga de CPU e latência). **Ainda não é a interface do rack**: é um teste para
medir o Galaxy S10 e validar o caminho Oboe → motor.

## Compilar pelo GitHub (sem PC e sem Android Studio)
O arquivo `.github/workflows/build.yml` faz o GitHub compilar o APK e rodar os testes. Pelo celular:
1. No GitHub, crie um repositório novo (público, para não gastar a cota de minutos).
2. **Add file → Upload files** e envie só o `synth-modular.zip`. Se o site não mostrar o botão, abra a página em
   "Versão para computador" no navegador. Faça o commit.
3. **Add file → Create new file**, escreva o nome `.github/workflows/build.yml`, cole o conteúdo do `build.yml`
   que enviei e faça o commit. Esse commit já dispara a compilação.
4. Aba **Actions → Build Android**: a compilação leva uns 5 a 10 minutos. No fim, em **Artifacts**, baixe
   `synth-modular-debug-apk`. Extraia o zip, abra o `app-debug.apk` e instale (o Android vai pedir para
   permitir instalar apps desconhecidos; o Play Protect pode avisar, é normal para um APK de teste).
5. Se a compilação falhar: abra a execução, o job `apk` e o passo "Compila o APK de debug", copie o texto do
   erro e me mande.

## Requisitos (para compilar localmente)
- Um **PC** com o [Android Studio](https://developer.android.com/studio) atual (Windows serve). Compilar este
  projeto direto no celular não é prático.
- Galaxy S10 com **Opções do desenvolvedor** e **Depuração USB** ligadas, e um cabo USB.

## Passo a passo
1. No Android Studio: **Open** e escolha a pasta `android-app`.
2. Quando ele pedir, aceite instalar o que faltar pelo SDK Manager: **SDK Platform 34**, **NDK** e
   **CMake 3.22.1**. O primeiro sync baixa o Gradle 8.9 e o Oboe 1.10.0 (precisa de internet).
3. Se ele reclamar do Gradle wrapper (os arquivos `gradlew` não vêm no projeto), use a opção para criar
   o wrapper, ou rode `gradle wrapper --gradle-version 8.9` numa pasta com Gradle instalado.
4. Conecte o S10, autorize a depuração no celular, escolha o aparelho na barra de cima e clique em **Run**.
5. No app, toque em **Ligar áudio**. Mexa nos sliders: o motor deve responder na hora.

## O que me mandar depois do primeiro teste
- Os números da tela: taxa, burst, buffer, xruns, **CPU do áudio**, latência, e se a API é AAudio e o
  modo exclusivo.
- O modelo exato do S10 (S10, S10+ ou S10e; Exynos ou Snapdragon) e a versão do Android.
- Qualquer erro de compilação (copie o texto inteiro do painel Build).

## O que foi verificado e o que não foi
- **Testado no PC (g++ 13):** o motor, o Stages (147 verificações) e o patch deste app (11 verificações:
  áudio, andamento, volume, parâmetros inválidos, LEDs, reconstrução em outra taxa).
- **Só checado de sintaxe:** o `native-lib.cpp` foi compilado contra *stubs* do Oboe, do JNI e do log que eu
  escrevi a partir da documentação. Isso pega erros de digitação, mas não prova que a API real é igual.
- **Não testado:** o workflow do GitHub Actions (não tenho como rodá-lo aqui). Ele usa o Android SDK que os
  servidores do GitHub já trazem instalado; se faltar algum componente, o Gradle tenta baixar.
- **Não compilado:** os arquivos Gradle, o Java e o CMake (não havia Android SDK, NDK, javac nem cmake no
  ambiente onde escrevi). Versões escolhidas: Android Gradle Plugin 8.7.3, Gradle 8.9, compileSdk 34,
  minSdk 26, Oboe 1.10.0 (confirmei que a 1.10.0 existe; se houver uma estável mais nova, pode trocar em
  `app/build.gradle.kts`).

## Atualizar o motor dentro do projeto
Os headers em `app/src/main/cpp/engine/` são cópias de `../src`. Depois de mudar o motor:

```
cp src/engine.h src/modules.h src/stages.h src/synth_patch.h android-app/app/src/main/cpp/engine/
```

## Limites desta versão
Patch fixo (sem montar cabos), sem o Plaits, sem a interface do rack, e o áudio para quando o app sai da tela.
