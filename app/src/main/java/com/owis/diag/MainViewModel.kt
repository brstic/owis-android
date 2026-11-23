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
    val liveHistory: Map<String, List<Float>> = emptyMap(),
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
            if (values.isNotEmpty()) {
                val row = StringBuilder(System.currentTimeMillis().toString())
                _ui.update { st ->
                    val m = st.live.toMutableMap()
                    val h = st.liveHistory.toMutableMap()
                    for (i in values.indices) {
                        val name = names.getOrElse(i) { "#$i" }
                        m[name] = values[i]
                        h[name] = ((h[name] ?: emptyList()) + values[i]).takeLast(120)
                        row.append(',').append(values[i])
                    }
                    st.copy(live = m, liveHistory = h)
                }
                synchronized(csvRows) { csvRows.add(row.toString()) }
            }
        },
    )

    private val csvRows = mutableListOf<String>()

    /** Write the accumulated live readings to a CSV file and return it (null if nothing recorded). */
    fun exportLiveCsv(): File? {
        val rows = synchronized(csvRows) { csvRows.toList() }
        if (rows.isEmpty()) return null
        val names = _ui.value.liveHistory.keys.toList()
        val dir = File(getApplication<Application>().getExternalFilesDir(null), "reports").apply { mkdirs() }
        val f = File(dir, "OWIS_live_${System.currentTimeMillis()}.csv")
        f.bufferedWriter().use { w ->
            w.write("time_ms," + names.joinToString(","){ "\"" + it.replace("\"","'") + "\"" }); w.newLine()
            rows.forEach { w.write(it); w.newLine() }
        }
        return f
    }

    private fun dataDir(): File = File(getApplication<Application>().getExternalFilesDir(null), "data").apply { mkdirs() }
    private fun keyFile(): File = File(dataDir(), "owis.keys")
    fun reportsDir(): File = File(getApplication<Application>().getExternalFilesDir(null), "reports").apply { mkdirs() }

    @Volatile private var logFile: File? = null
    private fun writeLogLine(line: String) {
        val f = logFile ?: return
        runCatching { synchronized(f) { f.appendText(line + "\n") } }
    }

    /** All saved session files (traces, logs, reports, CSVs), newest first — for sharing them off the phone. */
    fun sessionFiles(): List<File> =
        reportsDir().listFiles()?.filter { it.isFile }?.sortedByDescending { it.lastModified() } ?: emptyList()

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

    private fun appendLog(line: String) {
        writeLogLine(line)
        _ui.update { val l = (it.log + line).takeLast(400); it.copy(log = l) }
    }

    private fun stamp() = java.text.SimpleDateFormat("yyyyMMdd_HHmmss", java.util.Locale.US).format(java.util.Date())

    fun connect(device: UsbDevice) {
        if (_ui.value.busy) return
        _ui.update { it.copy(busy = true, status = "Connecting…", scan = emptyList(), live = emptyMap()) }
        viewModelScope.launch(Dispatchers.IO) {
            if (!usb.open(device)) { set(busy = false, status = "Could not open the adapter"); return@launch }
            appendLog("Adapter opened. Handshaking with the OpenPort 2.0…")
            if (!Native.nativeConnect()) { usb.close(); set(busy = false, status = "Adapter did not respond"); return@launch }
            // start a persistent log file and auto-record the whole session, so logs are kept on the phone
            val ts = stamp()
            logFile = File(reportsDir(), "OWIS_log_$ts.txt").also { runCatching { it.writeText("OWIS session $ts\n") } }
            val trace = File(reportsDir(), "OWIS_trace_$ts.ptrace")
            if (Native.nativeStartRecord(trace.absolutePath)) _ui.update { it.copy(recording = true) }
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
        Native.nativeDisconnect(); usb.close(); logFile = null
        _ui.update { it.copy(connected = false, recording = false, vehicle = null, scan = emptyList(), live = emptyMap(), status = "Not connected") }
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
        synchronized(csvRows) { csvRows.clear() }
        _ui.update { it.copy(live = emptyMap(), liveHistory = emptyMap()) }
        Native.nativeLive(moduleIndex, sel, rounds)
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
