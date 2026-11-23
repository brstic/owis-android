package com.owis.diag

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.activity.viewModels
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.unit.dp
import androidx.compose.runtime.getValue
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.owis.diag.ui.screen.*
import com.owis.diag.ui.theme.OwisTheme
import java.io.File

private const val ACTION_USB_PERMISSION = "com.owis.diag.USB_PERMISSION"

class MainActivity : ComponentActivity() {
    private val vm: MainViewModel by viewModels()

    private val usbReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != ACTION_USB_PERMISSION) return
            val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
            val device: UsbDevice? = if (Build.VERSION.SDK_INT >= 33)
                intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else @Suppress("DEPRECATION") intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            if (granted && device != null) vm.connect(device)
            else toast("USB permission denied")
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val filter = IntentFilter(ACTION_USB_PERMISSION)
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(usbReceiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(usbReceiver, filter)

        setContent {
            val ui by vm.ui.collectAsStateWithLifecycle()
            val importer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
                if (uri != null) importData(uri)
            }
            val replayer = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri ->
                if (uri != null) replaySession(uri)
            }
            OwisTheme {
                AppScaffold(vm, ui,
                    onConnect = ::startConnect,
                    onImport = { importer.launch(null) },
                    onReplay = { replayer.launch(arrayOf("*/*")) },
                    onShareReport = { shareReport(ui.reportHtml) })
            }
        }
    }

    private fun replaySession(uri: Uri) {
        val tmp = File(cacheDir, "replay.ptrace")
        runCatching { contentResolver.openInputStream(uri)?.use { it.copyTo(tmp.outputStream()) } }
        if (tmp.exists() && tmp.length() > 0) vm.replay(tmp) else toast("Could not read the trace file")
    }

    private fun shareReport(htmlPath: String) {
        if (htmlPath.isEmpty() || !File(htmlPath).exists()) { toast("Scan the car first to make a report"); return }
        val files = listOf(File(htmlPath), File(htmlPath.removeSuffix(".html") + ".txt")).filter { it.exists() }
        val uris = ArrayList(files.map { androidx.core.content.FileProvider.getUriForFile(this, "$packageName.files", it) })
        val send = Intent(Intent.ACTION_SEND_MULTIPLE).setType("*/*")
            .putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris)
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        startActivity(Intent.createChooser(send, "Share report (HTML + text)"))
    }

    override fun onDestroy() { super.onDestroy(); runCatching { unregisterReceiver(usbReceiver) } }

    private fun startConnect() {
        val d = vm.usb.findDevice() ?: run { toast("No OpenPort 2.0 found. Plug it into the USB-OTG cable."); return }
        if (vm.usb.hasPermission(d)) { vm.connect(d); return }
        val flags = if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0
        val pi = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERMISSION).setPackage(packageName), flags)
        vm.usb.requestPermission(d, pi)
    }

    private fun importData(tree: Uri) {
        val root = DocumentFile.fromTreeUri(this, tree) ?: return
        val dst = File(getExternalFilesDir(null), "data").apply { mkdirs() }
        var n = 0
        root.listFiles().forEach { f ->
            val name = f.name ?: return@forEach
            if (name.endsWith(".json") || name == "owis.keys") {
                runCatching {
                    contentResolver.openInputStream(f.uri)?.use { input ->
                        File(dst, name).outputStream().use { input.copyTo(it) }; n++
                    }
                }
            }
        }
        toast(if (n > 0) "Imported $n files" else "No .json or owis.keys found in that folder")
        vm.refreshData()
    }

    private fun toast(s: String) = Toast.makeText(this, s, Toast.LENGTH_LONG).show()
}

private enum class Tab(val label: String, val icon: ImageVector) {
    Home("Home", Icons.Filled.DirectionsCar),
    Faults("Faults", Icons.Filled.Search),
    Module("Module", Icons.Filled.Tune),
    Jobs("Jobs", Icons.Filled.Build),
    Log("Log", Icons.Filled.Terminal),
}

@Composable
private fun AppScaffold(
    vm: MainViewModel, ui: UiState,
    onConnect: () -> Unit, onImport: () -> Unit, onReplay: () -> Unit, onShareReport: () -> Unit,
) {
    var tab by remember { mutableStateOf(Tab.Home) }
    Scaffold(
        bottomBar = {
            NavigationBar {
                Tab.entries.forEach { t ->
                    NavigationBarItem(
                        selected = tab == t,
                        onClick = { tab = t },
                        icon = { Icon(t.icon, t.label) },
                        label = { Text(t.label) },
                    )
                }
            }
        }
    ) { pad ->
        Box(Modifier.padding(pad)) {
            when (tab) {
                Tab.Home -> HomeScreen(ui, vm, onConnect = onConnect, onImport = onImport,
                    onScan = { vm.scan(); tab = Tab.Faults }, onJobs = { tab = Tab.Jobs },
                    onModule = { tab = Tab.Module }, onReplay = onReplay, onShareReport = onShareReport)
                Tab.Faults -> ScanScreen(ui, onScan = vm::scan, onClearAll = vm::clearAll)
                Tab.Module -> ModuleScreen(vm, ui)
                Tab.Jobs -> JobsScreen(ui, onRun = vm::runJob)
                Tab.Log -> LogScreen(ui, onStop = vm::stop)
            }
        }
    }
}
