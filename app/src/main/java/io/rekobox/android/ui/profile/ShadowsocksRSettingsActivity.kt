package io.rekobox.android.ui.profile

import android.os.Bundle
import androidx.preference.EditTextPreference
import androidx.preference.PreferenceFragmentCompat
import io.rekobox.android.Key
import io.rekobox.android.R
import io.rekobox.android.database.DataStore
import io.rekobox.android.database.preference.EditTextPreferenceModifiers
import io.rekobox.android.fmt.shadowsocksr.ShadowsocksRBean

class ShadowsocksRSettingsActivity : ProfileSettingsActivity<ShadowsocksRBean>() {

    override fun createEntity() = ShadowsocksRBean()

    override fun ShadowsocksRBean.init() {
        DataStore.profileName = name
        DataStore.serverAddress = serverAddress
        DataStore.serverPort = serverPort
        DataStore.serverMethod = method
        DataStore.serverPassword = password
        DataStore.serverProtocol = protocol
        DataStore.serverObfs = obfs
        DataStore.serverProtocolParam = protocolParam
        DataStore.serverObfsParam = obfsParam
    }

    override fun ShadowsocksRBean.serialize() {
        name = DataStore.profileName
        serverAddress = DataStore.serverAddress
        serverPort = DataStore.serverPort
        method = DataStore.serverMethod
        password = DataStore.serverPassword
        protocol = DataStore.serverProtocol
        obfs = DataStore.serverObfs
        protocolParam = DataStore.serverProtocolParam
        obfsParam = DataStore.serverObfsParam
    }

    override fun PreferenceFragmentCompat.createPreferences(
        savedInstanceState: Bundle?,
        rootKey: String?,
    ) {
        addPreferencesFromResource(R.xml.shadowsocksr_preferences)
        findPreference<EditTextPreference>(Key.SERVER_PORT)!!.apply {
            setOnBindEditTextListener(EditTextPreferenceModifiers.Port)
        }
        findPreference<EditTextPreference>(Key.SERVER_PASSWORD)!!.apply {
            summaryProvider = PasswordSummaryProvider
        }
    }
}
