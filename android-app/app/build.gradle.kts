plugins {
    id("com.android.application")
}

android {
    namespace = "com.example.synthmodular"
    compileSdk = 34

    defaultConfig {
        applicationId = "com.example.synthmodular"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1"

        ndk {
            // Galaxy S10 = arm64-v8a. x86_64 serve para o emulador.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                cppFlags += "-std=c++17"
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildFeatures {
        prefab = true   // o Oboe vem como pacote prefab do Google Maven
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

dependencies {
    implementation("com.google.oboe:oboe:1.10.0")
}
