package com.owis.diag

/** Called from native code (on the worker thread) with log lines, function output and live values. */
class NativeCallbacks(
    private val log: (String) -> Unit,
    private val out: (String) -> Unit,
    private val live: (String, FloatArray, Array<String>) -> Unit,
) {
    fun onLog(s: String) = log(s)
    fun onOut(s: String) = out(s)
    fun onLive(fn: String, values: FloatArray, names: Array<String>) = live(fn, values, names)
}
