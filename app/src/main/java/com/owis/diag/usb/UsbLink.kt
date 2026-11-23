package com.owis.diag.usb

/** Raw byte pipe to the adapter, called from native code. */
interface UsbLink {
    /** Write all bytes; returns the number written, or <0 on error. */
    fun write(data: ByteArray): Int
    /** Read up to [max] bytes within [timeoutMs]; returns the bytes read (empty on timeout). */
    fun read(max: Int, timeoutMs: Int): ByteArray
}
