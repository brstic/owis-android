package com.owis.diag.ui.screen

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.owis.diag.UiState
import com.owis.diag.data.ModuleScan
import com.owis.diag.ui.*
import com.owis.diag.ui.theme.*

@Composable
fun ScanScreen(ui: UiState, onScan: () -> Unit, onClearAll: () -> Unit) {
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(16.dp), verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text("Fault memory", style = MaterialTheme.typography.titleLarge, color = OnBg)
                val faults = ui.scan.sumOf { it.faults.size }
                Text(if (ui.scan.isEmpty()) "Not scanned yet" else "${ui.scan.size} units • $faults faults",
                    style = MaterialTheme.typography.bodyMedium, color = OnMuted)
            }
            val live = ui.connected || ui.replaying
            FilledTonalButton(onClick = onClearAll, enabled = ui.connected && !ui.busy) {
                Icon(Icons.Filled.DeleteSweep, null, Modifier.size(18.dp)); Spacer(Modifier.width(6.dp)); Text("Clear")
            }
            Spacer(Modifier.width(8.dp))
            Button(onClick = onScan, enabled = live && !ui.busy) {
                Icon(Icons.Filled.Search, null, Modifier.size(18.dp)); Spacer(Modifier.width(6.dp)); Text("Scan")
            }
        }
        if (ui.busy && ui.scan.isEmpty()) LinearProgressIndicator(Modifier.fillMaxWidth(), color = Accent)
        LazyColumn(Modifier.fillMaxSize().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            items(ui.scan) { m -> ModuleCard(m) }
            item { Spacer(Modifier.height(24.dp)) }
        }
    }
}

@Composable
private fun ModuleCard(m: ModuleScan) {
    var open by remember(m) { mutableStateOf(m.faults.any { it.status.contains("active", true) }) }
    Card {
        Row(Modifier.clickable { open = !open }, verticalAlignment = Alignment.CenterVertically) {
            Column(Modifier.weight(1f)) {
                Text(m.name, style = MaterialTheme.typography.titleMedium, color = OnBg)
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    if (m.proto.isNotEmpty()) Chip(m.proto, Info)
                    Text(String.format("0x%03X", m.id), style = MaterialTheme.typography.bodySmall, color = OnMuted)
                }
            }
            if (m.faults.isEmpty()) Chip("OK", Accent)
            else Chip("${m.faults.size} fault" + if (m.faults.size > 1) "s" else "", Danger)
            Spacer(Modifier.width(8.dp))
            Icon(if (open) Icons.Filled.ExpandLess else Icons.Filled.ExpandMore, null, tint = OnMuted)
        }
        AnimatedVisibility(open) {
            Column(Modifier.padding(top = 10.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
                if (m.ident.isNotEmpty()) Mono(m.ident)
                m.faults.forEach { f ->
                    Column {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Mono(f.code, color = OnBg)
                            Spacer(Modifier.width(8.dp))
                            Chip(f.status, statusColor(f.status))
                        }
                        Text(f.text, style = MaterialTheme.typography.bodyMedium, color = OnBg, modifier = Modifier.padding(top = 2.dp))
                        if (f.env.isNotEmpty()) Mono(f.env, modifier = Modifier.padding(top = 2.dp))
                    }
                    HorizontalDivider(color = Outline)
                }
            }
        }
    }
}
