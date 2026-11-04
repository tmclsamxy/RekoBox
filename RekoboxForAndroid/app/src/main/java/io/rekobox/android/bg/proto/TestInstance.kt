package io.rekobox.android.bg.proto

import io.rekobox.android.BuildConfig
import io.rekobox.android.bg.GuardedProcessPool
import io.rekobox.android.database.ProxyEntity
import io.rekobox.android.fmt.buildConfig
import io.rekobox.android.ktx.Logs
import io.rekobox.android.ktx.runOnDefaultDispatcher
import io.rekobox.android.ktx.tryResume
import io.rekobox.android.ktx.tryResumeWithException
import kotlinx.coroutines.delay
import libcore.Libcore
import io.rekobox.android.nb4a.net.LocalResolverImpl
import kotlin.coroutines.suspendCoroutine

class TestInstance(profile: ProxyEntity, val link: String, private val timeout: Int) :
    BoxInstance(profile) {

    suspend fun doTest(): Int {
        return suspendCoroutine { c ->
            processes = GuardedProcessPool {
                Logs.w(it)
                c.tryResumeWithException(it)
            }
            runOnDefaultDispatcher {
                use {
                    try {
                        init()
                        launch()
                        if (processes.processCount > 0) {
                            // wait for plugin start
                            delay(500)
                        }
                        c.tryResume(Libcore.urlTest(box, link, timeout))
                    } catch (e: Exception) {
                        c.tryResumeWithException(e)
                    }
                }
            }
        }
    }

    override fun buildConfig() {
        config = buildConfig(profile, true)
    }

    override suspend fun loadConfig() {
        // don't call destroyAllJsi here
        if (BuildConfig.DEBUG) Logs.d(config.config)
        box = Libcore.newSingBoxInstance(config.config, LocalResolverImpl)
    }

}
