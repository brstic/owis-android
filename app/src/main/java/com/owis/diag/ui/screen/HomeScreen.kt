package com.owis.diag.ui.screen

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.owis.diag.MainViewModel
import com.owis.diag.UiState
import com.owis.diag.ui.*
import com.owis.diag.ui.theme.*

@Composable
fun HomeScreen(
    ui: UiState,
    vm: MainViewModel,
    onConnect: () -> Unit,
    onImport: () -> Unit,
    onScan: () -> Unit,
    onJobs: () -> Unit,
    onModule: () -> Unit,
    onReplay: () -> Unit,
    onShareReport: () -> Unit,
    onShareLogs: () -> Unit = {},
) {
    Column(
        Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically) {
            Text("OWIS", style = MaterialTheme.typography.headlineMedium, color = OnBg)
            Spacer(Modifier.width(8.dp))
            Text("diagnostics", style = MaterialTheme.typography.titleMedium, color = Accent)
        }

        Card {
            Row(verticalAlignment = Alignment.CenterVertically) {
                StatusDot(ok = ui.connected || ui.replaying, warn = ui.busy)
                Spacer(Modifier.width(10.dp))
                Column(Modifier.weight(1f)) {
                    Text(when { ui.replaying -> "Replay"; ui.connected -> "Connected"; else -> "Adapter" },
                        style = MaterialTheme.typography.titleMedium, color = OnBg)
                    Text(ui.status, style = MaterialTheme.typography.bodyMedium, color = OnMuted)
                }
                if (ui.busy) CircularProgressIndicator(Modifier.size(22.dp), color = Accent, strokeWidth = 2.dp)
            }
            Spacer(Modifier.height(14.dp))
            if (!ui.connected) {
                Button(onClick = onConnect, enabled = ui.dataReady && !ui.busy,
                    modifier = Modifier.fillMaxWidth().height(50.dp), shape = RoundedCornerShape(14.dp)) {
                    Icon(Icons.Filled.Usb, null); Spacer(Modifier.width(8.dp)); Text("Connect to the car", fontWeight = FontWeight.SemiBold)
                }
                Spacer(Modifier.height(8.dp))
                OutlinedButton(onClick = onReplay, modifier = Modifier.fillMaxWidth().height(46.dp), shape = RoundedCornerShape(14.dp)) {
                    Icon(Icons.Filled.History, null); Spacer(Modifier.width(8.dp)); Text("Replay a recorded session")
                }
            } else {
                OutlinedButton(onClick = vm::disconnect, modifier = Modifier.fillMaxWidth().height(48.dp), shape = RoundedCornerShape(14.dp)) {
                    Icon(Icons.Filled.LinkOff, null); Spacer(Modifier.width(8.dp)); Text("Disconnect")
                }
            }
        }

        ui.vehicle?.let { v ->
            Card {
                SectionTitle("VEHICLE")
                Spacer(Modifier.height(6.dp))
                Text(v.title, style = MaterialTheme.typography.titleLarge, color = OnBg)
                if (v.vin.isNotEmpty()) { Spacer(Modifier.height(4.dp)); Mono("VIN  ${v.vin}") }
            }
        }

        if (!ui.dataReady) {
            Card {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Icon(Icons.Filled.Info, null, tint = Warn); Spacer(Modifier.width(10.dp))
                    Text("No vehicle data yet", style = MaterialTheme.typography.titleMedium, color = OnBg)
                }
                Spacer(Modifier.height(8.dp))
                Text("OWIS ships no vehicle data. Build it on a PC from your own licensed installation " +
                    "(build_data.py), then import the data folder and owis.keys here.",
                    style = MaterialTheme.typography.bodyMedium, color = OnMuted)
                Spacer(Modifier.height(12.dp))
                Button(onClick = onImport, modifier = Modifier.fillMaxWidth().height(48.dp), shape = RoundedCornerShape(14.dp)) {
                    Icon(Icons.Filled.FolderOpen, null); Spacer(Modifier.width(8.dp)); Text("Import data folder")
                }
            }
        } else {
            Card {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        SectionTitle("DATA")
                        Spacer(Modifier.height(4.dp))
                        Text("${ui.packs.size} vehicle lines", style = MaterialTheme.typography.titleMedium, color = OnBg)
                    }
                    Chip(if (ui.keyCount > 0) "coding: ${ui.keyCount} keys" else "no coding keys",
                        color = if (ui.keyCount > 0) Accent else OnMuted)
                }
            }
        }

        if (ui.connected || ui.replaying) {
            Text("ACTIONS", style = MaterialTheme.typography.labelSmall, color = OnMuted)
            ActionRow(Icons.Filled.Search, "Scan all control units", "Read and clear faults", onScan)
            ActionRow(Icons.Filled.Tune, "Open a control unit", "Tests, routines, coding, live data", onModule)
            ActionRow(Icons.Filled.Build, "Guided jobs", "Service, calibrations, PDK", onJobs)
            if (ui.reportHtml.isNotEmpty())
                ActionRow(Icons.Filled.Share, "Share the scan report", "HTML, printable", onShareReport)
        }

        // logs are always saved on the phone; share them off after testing on the car
        ActionRow(Icons.Filled.Upload, "Share logs & traces", "Send the saved session files off the phone", onShareLogs)
        if (ui.recording) Text("● Recording this session (saved on the phone)",
            style = MaterialTheme.typography.labelSmall, color = Accent, modifier = Modifier.padding(start = 4.dp))

        // session options
        Card {
            SectionTitle("SESSION")
            Spacer(Modifier.height(8.dp))
            Row(verticalAlignment = Alignment.CenterVertically) {
                Column(Modifier.weight(1f)) {
                    Text("Redact VIN in reports", style = MaterialTheme.typography.bodyLarge, color = OnBg)
                    Text("Hide the serial in the HTML report", style = MaterialTheme.typography.bodyMedium, color = OnMuted)
                }
                Switch(checked = ui.redactVin, onCheckedChange = { vm.setRedactVin(it) })
            }
            if (ui.connected) {
                Spacer(Modifier.height(10.dp))
                OutlinedButton(onClick = vm::toggleRecord, modifier = Modifier.fillMaxWidth(), shape = RoundedCornerShape(14.dp)) {
                    Icon(if (ui.recording) Icons.Filled.StopCircle else Icons.Filled.FiberManualRecord, null,
                        tint = if (ui.recording) Danger else Accent)
                    Spacer(Modifier.width(8.dp)); Text(if (ui.recording) "Stop recording" else "Record this session")
                }
            }
        }
        Spacer(Modifier.height(24.dp))
    }
}

@Composable
private fun ActionRow(icon: androidx.compose.ui.graphics.vector.ImageVector, title: String, sub: String, onClick: () -> Unit) {
    Surface(onClick = onClick, shape = RoundedCornerShape(16.dp), color = Surface1,
        border = androidx.compose.foundation.BorderStroke(1.dp, Outline), modifier = Modifier.fillMaxWidth()) {
        Row(Modifier.padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Icon(icon, null, tint = Accent)
            Spacer(Modifier.width(14.dp))
            Column(Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.titleMedium, color = OnBg)
                Text(sub, style = MaterialTheme.typography.bodyMedium, color = OnMuted)
            }
            Icon(Icons.Filled.ChevronRight, null, tint = OnMuted)
        }
    }
}
