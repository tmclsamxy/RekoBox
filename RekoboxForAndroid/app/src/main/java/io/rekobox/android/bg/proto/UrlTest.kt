package io.rekobox.android.bg.proto

import io.rekobox.android.database.DataStore
import io.rekobox.android.database.ProxyEntity

class UrlTest {

    val link = DataStore.connectionTestURL
    private val timeout = DataStore.connectionTestTimeout

    suspend fun doTest(profile: ProxyEntity): Int {
        return TestInstance(profile, link, timeout).doTest()
    }

}
