package io.rekobox.android.ui

import android.os.Bundle
import android.view.View
import android.widget.Button
import android.widget.EditText
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import io.rekobox.android.R
import io.rekobox.android.sync.SyncEngine
import io.rekobox.android.sync.SyncStore

/**
 * 账号与同步：登录 / 注销 / 立即同步 / 自动同步开关。
 * 协议与 Windows 端一致，服务端同一套接口，账号由服务端管理员创建（不提供注册）。
 */
class AccountFragment : ToolbarFragment(R.layout.layout_account) {

    private lateinit var textAccount: TextView
    private lateinit var textStatus: TextView
    private lateinit var editServer: EditText
    private lateinit var editUser: EditText
    private lateinit var editPass: EditText
    private lateinit var btnLogin: Button
    private lateinit var btnSync: Button
    private lateinit var btnLogout: Button
    private lateinit var switchAuto: Switch

    private var binding = false

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        toolbar.setTitle(R.string.menu_account)

        textAccount = view.findViewById(R.id.textAccount)
        textStatus = view.findViewById(R.id.textStatus)
        editServer = view.findViewById(R.id.editServer)
        editUser = view.findViewById(R.id.editUser)
        editPass = view.findViewById(R.id.editPass)
        btnLogin = view.findViewById(R.id.btnLogin)
        btnSync = view.findViewById(R.id.btnSync)
        btnLogout = view.findViewById(R.id.btnLogout)
        switchAuto = view.findViewById(R.id.switchAutoSync)

        SyncEngine.init()
        editServer.setText(SyncStore.server)
        editUser.setText(SyncStore.username)
        editServer.setHint(R.string.account_server_hint)

        btnLogin.setOnClickListener { onLogin() }
        btnSync.setOnClickListener {
            SyncEngine.syncNow("manual")
            Toast.makeText(requireContext(), R.string.account_syncing, Toast.LENGTH_SHORT).show()
            refresh()
        }
        btnLogout.setOnClickListener { onLogout() }
        switchAuto.isChecked = SyncStore.autoSync
        switchAuto.setOnCheckedChangeListener { _, checked -> SyncStore.autoSync = checked }

        SyncEngine.onChange = {
            activity?.runOnUiThread { if (isAdded) refresh() }
        }
        refresh()
    }

    override fun onDestroyView() {
        SyncEngine.onChange = null
        super.onDestroyView()
    }

    private fun onLogin() {
        if (binding) return
        val server = editServer.text.toString()
        val user = editUser.text.toString()
        val pass = editPass.text.toString()
        setBusy(true)
        textStatus.setText(R.string.account_logging_in)

        SyncEngine.login(server, user, pass) { ok, message ->
            activity?.runOnUiThread {
                if (!isAdded) return@runOnUiThread
                setBusy(false)
                editPass.setText("")
                if (!ok) {
                    textStatus.text = message
                    Toast.makeText(requireContext(), message, Toast.LENGTH_LONG).show()
                }
                refresh()
            }
        }
    }

    private fun onLogout() {
        if (!SyncEngine.loggedIn()) return
        setBusy(true)
        SyncEngine.logout {
            activity?.runOnUiThread {
                if (!isAdded) return@runOnUiThread
                setBusy(false)
                Toast.makeText(requireContext(), R.string.account_logged_out, Toast.LENGTH_SHORT).show()
                refresh()
            }
        }
    }

    private fun setBusy(busy: Boolean) {
        btnLogin.isEnabled = !busy
        btnSync.isEnabled = !busy
        btnLogout.isEnabled = !busy
        editServer.isEnabled = !busy
        editUser.isEnabled = !busy
        editPass.isEnabled = !busy
    }

    private fun refresh() {
        if (!this::textStatus.isInitialized) return
        val logged = SyncEngine.loggedIn()
        textAccount.text = if (logged) {
            getString(R.string.account_current, SyncStore.username, SyncStore.server)
        } else {
            getString(R.string.account_not_logged_in)
        }
        textStatus.text = SyncEngine.statusText()

        btnLogin.visibility = if (logged) View.GONE else View.VISIBLE
        editPass.visibility = if (logged) View.GONE else View.VISIBLE
        editServer.isEnabled = !logged
        editUser.isEnabled = !logged
        btnLogout.isEnabled = logged
        btnSync.isEnabled = logged && SyncEngine.state != SyncEngine.State.SYNCING
        switchAuto.isEnabled = logged
    }
}
