// No libraries at all: the app uses only the system WebView, so the APK stays tiny (about 100-200 KB after shrinking).
plugins { id("com.android.application") }
android {
    namespace = "studio.homebax.browser"
    compileSdk = 34
    defaultConfig {
        applicationId = "studio.homebax.browser"
        minSdk = 24
        targetSdk = 34
        versionCode = 30100
        versionName = "3.1.0"
    }
    buildTypes {
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
        }
    }
    compileOptions { sourceCompatibility = JavaVersion.VERSION_17; targetCompatibility = JavaVersion.VERSION_17 }
    buildFeatures { buildConfig = true }
}
