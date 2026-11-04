package io.rekobox.android.bg

import java.io.Closeable

interface AbstractInstance : Closeable {

    fun launch()

}