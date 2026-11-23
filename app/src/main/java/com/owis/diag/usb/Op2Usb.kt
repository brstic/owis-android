package com.owis.diag.usb

import android.app.PendingIntent
import android.content.Context
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager

/** Tactrix OpenPort 2.0 over USB (CDC-ACM data interface, bulk EP 0x02 OUT / 0x82 IN). */
class Op2Usb(context: Context) : UsbLink {
    private val usb = context.getSystemService(Context.USB_SERVICE) as UsbManager
    private var conn: UsbDeviceConnection? = null
    private var iface: UsbInterface? = null
    private var epIn: UsbEndpoint? = null
    private var epOut: UsbEndpoint? = null
    private val readBuf = ByteArray(4096)

    companion object {
        const val VID = 0x0403
        val PIDS = intArrayOf(0xCC4B, 0xCC4C, 0xCC4D)
    }

    fun findDevice(): UsbDevice? =
        usb.deviceList.values.firstOrNull { it.vendorId == VID && PIDS.contains(it.productId) }

    fun hasPermission(d: UsbDevice): Boolean = usb.hasPermission(d)
    fun requestPermission(d: UsbDevice, pi: PendingIntent) = usb.requestPermission(d, pi)
    val isOpen: Boolean get() = conn != null

    fun open(d: UsbDevice): Boolean {
        val c = usb.openDevice(d) ?: return false
        var chosen: UsbInterface? = null; var ei: UsbEndpoint? = null; var eo: UsbEndpoint? = null
        for (i in 0 until d.interfaceCount) {
            val intf = d.getInterface(i)
            var bin: UsbEndpoint? = null; var bout: UsbEndpoint? = null
            for (j in 0 until intf.endpointCount) {
                val ep = intf.getEndpoint(j)
                if (ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
                    if (ep.direction == UsbConstants.USB_DIR_IN) bin = ep else bout = ep
                }
            }
            if (bin != null && bout != null) { chosen = intf; ei = bin; eo = bout; break }
        }
        if (chosen == null) { c.close(); return false }
        if (!c.claimInterface(chosen, true)) { c.close(); return false }
        // CDC SET_CONTROL_LINE_STATE: assert DTR+RTS (0x03), like the vendor driver.
        runCatching { c.controlTransfer(0x21, 0x22, 0x03, 0, null, 0, 200) }
        conn = c; iface = chosen; epIn = ei; epOut = eo
        return true
    }

    fun close() {
        val c = conn
        iface?.let { runCatching { c?.releaseInterface(it) } }
        runCatching { c?.close() }
        conn = null; iface = null; epIn = null; epOut = null
    }

    override fun write(data: ByteArray): Int {
        val c = conn ?: return -1; val ep = epOut ?: return -1
        var off = 0
        while (off < data.size) {
            val chunk = if (off == 0) data else data.copyOfRange(off, data.size)
            val n = c.bulkTransfer(ep, chunk, chunk.size, 1000)
            if (n <= 0) return if (off > 0) off else -1
            off += n
        }
        return off
    }

    override fun read(max: Int, timeoutMs: Int): ByteArray {
        val c = conn ?: return ByteArray(0); val ep = epIn ?: return ByteArray(0)
        val n = c.bulkTransfer(ep, readBuf, minOf(max, readBuf.size), timeoutMs.coerceAtLeast(1))
        return if (n > 0) readBuf.copyOf(n) else ByteArray(0)
    }
}
