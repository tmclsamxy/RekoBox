import org.jetbrains.kotlin.gradle.dsl.JvmTarget

import org.jetbrains.compose.desktop.application.dsl.TargetFormat

plugins {
    kotlin("jvm")
    id("org.jetbrains.compose")
    id("org.jetbrains.kotlin.plugin.compose")
}

kotlin {
    compilerOptions { jvmTarget.set(JvmTarget.JVM_17) }
}

java {
    sourceCompatibility = JavaVersion.VERSION_17
    targetCompatibility = JavaVersion.VERSION_17
}

dependencies {
    implementation(project(":core"))
    implementation(compose.desktop.currentOs)
    implementation(compose.material3)
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-swing:1.9.0")
    implementation("org.jetbrains.kotlinx:kotlinx-serialization-json:1.7.3")
}

compose.desktop {
    application {
        mainClass = "io.rekobox.win.MainKt"

        nativeDistributions {
            // sing-box.exe / wintun.dll 不进 jar：由 jpackage 从 runtime/windows 复制到安装目录根，
            // 开发期 AppPaths 会自己往上找 runtime/windows。
            appResourcesRootDir.set(project.layout.projectDirectory.dir("../runtime"))

            targetFormats(TargetFormat.Msi)
            packageName = "Rekobox"
            packageVersion = "1.0.0"
            description = "Rekobox for Windows"
            vendor = "Rekobox"
            copyright = "GPL-3.0"
            // HttpClient 需要 java.net.http；utls/ECH 相关走 BoringSSL 但仍在核心进程里
            modules("java.net.http", "jdk.crypto.ec", "java.management")
            windows {
                menuGroup = "Rekobox"
                shortcut = true
                // TUN 需要管理员：装到 Program Files，不按用户安装
                dirChooser = true
                perUserInstall = false
                // 托盘常驻程序，装完不强制启动
                // （jpackage 没有 "run after install" 开关，MSI 默认不启动）
                upgradeUuid = "8f6f0d4c-1f0e-4a6e-9b1a-1a2b3c4d5e6f"
                iconFile.set(project.file("icon.ico"))
            }
        }
    }
}
