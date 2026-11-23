package com.owis.diag

import android.app.Application
import android.hardware.usb.UsbDevice
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.owis.diag.data.*
import com.owis.diag.usb.Op2Usb
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

data class UiState(
    val dataReady: Boolean = false,
    val keyCount: Int = 0,
    val packs: List<PackInfo> = emptyList(),
    val connected: Boolean = false,
    val busy: Boolean = false,
    val status: String = "Not connected",
    val vehicle: Vehicle? = null,
    val scan: List<ModuleScan> = emptyList(),
    val jobs: List<Job> = emptyList(),
    val log: List<String> = emptyList(),
    val live: Map<String, Float> = emptyMap(),
    val redactVin: Boolean = false,
    val recording: Boolean = false,
    val reportHtml: String = "",
    val replaying: Boolean = false,
)

class MainViewModel(app: Application) : AndroidViewModel(app) {
    val usb = Op2Usb(app)
    private val _ui = MutableStateFlow(UiState())
    val ui = _ui.asStateFlow()

    private val callbacks = NativeCallbacks(
        log = { line -> appendLog(line) },
        out = { line -> appendLog(line) },
        live = { _, values, names ->
            if (values.isNotEmpty()) _ui.update { st ->
                val m = st.live.toMutableMap(); for (i in values.indices) m[names.getOrElse(i) { "#$i" }] = values[i]; st.copy(live = m)
            }
        },
    )

    private fun dataDir(): File = File(getApplication<Application>().getExternalFilesDir(null), "data").apply { mkdirs() }
    private fun keyFile(): File = File(dataDir(), "owis.keys")

    init {
        Native.nativeRegister(usb, callbacks)
        refreshData()
    }

    fun refreshData() {
        viewModelScope.launch(Dispatchers.IO) {
            val dir = dataDir()
            val packFiles = dir.listFiles { f -> f.name.endsWith(".json") }?.map { it.absolutePath } ?: emptyList()
            val list = packFiles.joinToString("\n")
            val reportDir = File(getApplication<Application>().getExternalFilesDir(null), "reports").apply { mkdirs() }.absolutePath
            val packsJson = Native.nativeSetup(list, if (keyFile().exists()) keyFile().absolutePath else "", reportDir)
            val packs = runCatching { Parse.packs(packsJson) }.getOrDefault(emptyList())
            _ui.update { it.copy(dataReady = packs.isNotEmpty(), packs = packs, keyCount = Native.nativeKeyCount()) }
        }
    }

    private fun appendLog(line: String) = _ui.update {
        val l = (it.log + line).takeLast(400); it.copy(log = l)
    }

    fun connect(device: UsbDevice) {
        if (_ui.value.busy) return
        _ui.update { it.copy(busy = true, status = "Connecting…", scan = emptyList(), live = emptyMap()) }
        viewModelScope.launch(Dispatchers.IO) {
            if (!usb.open(device)) { set(busy = false, status = "Could not open the adapter"); return@launch }
            appendLog("Adapter opened. Handshaking with the OpenPort 2.0…")
            if (!Native.nativeConnect()) { usb.close(); set(busy = false, status = "Adapter did not respond"); return@launch }
            set(connected = true, status = "Reading the VIN…")
            val v = Parse.vehicle(Native.nativeDetect())
            if (v != null) {
                val jobs = runCatching { Parse.jobs(Native.nativeJobs()) }.getOrDefault(emptyList())
                _ui.update { it.copy(vehicle = v, jobs = jobs, status = v.title) }
            } else {
                set(status = "Connected — VIN not detected, pick a vehicle")
            }
            set(busy = false)
        }
    }

    fun loadPack(path: String) = io("Loading…") {
        if (Native.nativeLoadPack(path)) {
            val jobs = runCatching { Parse.jobs(Native.nativeJobs()) }.getOrDefault(emptyList())
            val title = _ui.value.packs.firstOrNull { it.path == path }?.title ?: "Vehicle"
            _ui.update { it.copy(vehicle = Vehicle(Native.nativeVin(), "", title), jobs = jobs, status = title) }
        }
    }

    fun scan() = io("Scanning all control units…") {
        val res = runCatching { Parse.scan(Native.nativeScan()) }.getOrDefault(emptyList())
        _ui.update { it.copy(scan = res, reportHtml = Native.nativeReportHtml(), status = "Scan: ${res.size} units answered") }
    }

    fun setRedactVin(on: Boolean) { Native.nativeSetRedactVin(on); _ui.update { it.copy(redactVin = on) } }

    fun toggleRecord() {
        if (_ui.value.recording) { Native.nativeStopRecord(); _ui.update { it.copy(recording = false) } }
        else {
            val dir = File(getApplication<Application>().getExternalFilesDir(null), "reports").apply { mkdirs() }
            val path = File(dir, "OWIS_trace_${System.currentTimeMillis()}.ptrace").absolutePath
            if (Native.nativeStartRecord(path)) _ui.update { it.copy(recording = true, status = "Recording session…") }
        }
    }

    fun replay(file: File) = io("Replaying session…") {
        if (Native.nativeReplay(file.absolutePath)) {
            val jobs = runCatching { Parse.jobs(Native.nativeJobs()) }.getOrDefault(emptyList())
            val v = Vehicle(Native.nativeVin(), "", "Replay")
            _ui.update { it.copy(replaying = true, vehicle = v, jobs = jobs, status = "Replaying ${file.name}") }
        }
    }

    fun clearAll() = io("Clearing all fault memories…") { Native.nativeClearAll(); }

    fun runJob(id: String) = io("Running job…") { Native.nativeRunJob(id, null) }

    fun disconnect() {
        Native.nativeDisconnect(); usb.close()
        _ui.update { it.copy(connected = false, vehicle = null, scan = emptyList(), live = emptyMap(), status = "Not connected") }
    }

    fun stop() = Native.nativeStop()

    fun modules(onReady: (List<ModuleRef>) -> Unit) = viewModelScope.launch(Dispatchers.IO) {
        onReady(runCatching { Parse.modules(Native.nativeModules()) }.getOrDefault(emptyList()))
    }
    fun functions(moduleIndex: Int, filter: String, onReady: (List<Fn>) -> Unit) = viewModelScope.launch(Dispatchers.IO) {
        onReady(runCatching { Parse.functions(Native.nativeFunctions(moduleIndex, filter)) }.getOrDefault(emptyList()))
    }
    fun runFunction(moduleIndex: Int, entry: Int) = io("Running…") { Native.nativeRun(moduleIndex, entry, null) }
    fun live(moduleIndex: Int, sel: IntArray, rounds: Int) = io("Reading live data…") {
        _ui.update { it.copy(live = emptyMap()) }; Native.nativeLive(moduleIndex, sel, rounds)
    }

    private fun io(status: String, block: suspend () -> Unit) {
        if (_ui.value.busy) return
        _ui.update { it.copy(busy = true, status = status) }
        viewModelScope.launch(Dispatchers.IO) { runCatching { block() }; set(busy = false) }
    }
    private fun set(connected: Boolean? = null, busy: Boolean? = null, status: String? = null) = _ui.update {
        it.copy(connected = connected ?: it.connected, busy = busy ?: it.busy, status = status ?: it.status)
    }
}
