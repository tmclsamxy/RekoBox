package io.rekobox.android.ui

import android.content.Intent
import android.os.Bundle
import android.view.View
import io.rekobox.android.R
import io.rekobox.android.databinding.LayoutNetworkBinding
import io.rekobox.android.ktx.app

class NetworkFragment : NamedFragment(R.layout.layout_network) {

    override fun name0() = app.getString(R.string.tools_network)

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)

        val binding = LayoutNetworkBinding.bind(view)
        binding.stunTest.setOnClickListener {
            startActivity(Intent(requireContext(), StunActivity::class.java))
        }
    }

}