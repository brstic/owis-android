package com.owis.diag

import com.owis.diag.usb.UsbLink

/** Thin JNI surface over the portable OWIS engine (libowis.so). */
object Native {
    init { System.loadLibrary("owis") }

    external fun nativeRegister(usb: UsbLink, cb: NativeCallbacks)
    external fun nativeSetup(packPaths: String, keyFile: String, reportDir: String): String
    external fun nativeKeyCount(): Int
    external fun nativeConnect(): Boolean
    external fun nativeDisconnect()
    external fun nativeLoadPack(path: String): Boolean
    external fun nativeDetect(): String
    external fun nativeScan(): String
    external fun nativeClearAll()
    external fun nativeModules(): String
    external fun nativeFunctions(moduleIndex: Int, filter: String): String
    external fun nativeRun(moduleIndex: Int, entry: Int, inputs: DoubleArray?): Boolean
    external fun nativeLive(moduleIndex: Int, sel: IntArray?, rounds: Int)
    external fun nativeStop()
    external fun nativeJobs(): String
    external fun nativeRunJob(id: String, inputs: DoubleArray?): Boolean
    external fun nativeVin(): String
    external fun nativeReportHtml(): String
}
