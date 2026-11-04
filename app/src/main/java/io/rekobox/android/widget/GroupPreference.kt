package io.rekobox.android.widget

import android.content.Context
import android.util.AttributeSet
import io.rekobox.android.R
import io.rekobox.android.database.SagerDatabase
import io.rekobox.android.nb4a.ui.SimpleMenuPreference

class GroupPreference
@JvmOverloads constructor(
    context: Context, attrs: AttributeSet? = null, defStyle: Int = R.attr.dropdownPreferenceStyle
) : SimpleMenuPreference(context, attrs, defStyle, 0) {

    init {
        val groups = SagerDatabase.groupDao.allGroups()

        entries = groups.map { it.displayName() }.toTypedArray()
        entryValues = groups.map { "${it.id}" }.toTypedArray()
    }

    override fun getSummary(): CharSequence? {
        if (!value.isNullOrBlank() && value != "0") {
            return SagerDatabase.groupDao.getById(value.toLong())?.displayName()
                ?: super.getSummary()
        }
        return super.getSummary()
    }

}