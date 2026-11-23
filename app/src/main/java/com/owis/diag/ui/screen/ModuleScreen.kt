package com.owis.diag.ui.screen

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyHorizontalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.ui.draw.clip
import androidx.compose.material.icons.filled.Share
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import com.owis.diag.MainViewModel
import com.owis.diag.UiState
import com.owis.diag.data.Fn
import com.owis.diag.data.ModuleRef
import com.owis.diag.ui.Card
import com.owis.diag.ui.Chip
import com.owis.diag.ui.theme.*

private fun catLabel(c: String) = when (c) {
    "R" -> "read"; "A" -> "actuator"; "U", "F" -> "routine"; "C" -> "coding"; else -> "function"
}
private fun catColor(c: String): Color = when (c) {
    "R" -> Info; "A" -> Warn; "U", "F" -> Accent; "C" -> Danger; else -> OnMuted
}
private fun isWrite(c: String) = c == "A" || c == "U" || c == "F" || c == "C"

@OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
@Composable
fun ModuleScreen(vm: MainViewModel, ui: UiState, onShareCsv: () -> Unit = {}) {
    var modules by remember { mutableStateOf<List<ModuleRef>>(emptyList()) }
    var module by remember { mutableStateOf<ModuleRef?>(null) }
    var fns by remember { mutableStateOf<List<Fn>>(emptyList()) }
    var filter by remember { mutableStateOf("") }
    var picking by remember { mutableStateOf(false) }
    var confirm by remember { mutableStateOf<Fn?>(null) }

    LaunchedEffect(ui.vehicle) { if (ui.vehicle != null) vm.modules { modules = it } }

    Column(Modifier.fillMaxSize().padding(16.dp)) {
        Text("Control unit", style = MaterialTheme.typography.titleLarge, color = OnBg)
        Spacer(Modifier.height(12.dp))
        Box {
            OutlinedButton(onClick = { picking = true }, Modifier.fillMaxWidth()) {
                Text(module?.name ?: "Choose a control unit", Modifier.weight(1f), color = OnBg)
                Icon(Icons.Filled.ExpandMore, null)
            }
            DropdownMenu(expanded = picking, onDismissRequest = { picking = false }) {
                modules.forEach { m ->
                    DropdownMenuItem(text = { Text(m.name) }, onClick = {
                        module = m; picking = false; filter = ""
                        vm.functions(m.index, "") { fns = it }
                    })
                }
            }
        }

        if (module != null) {
            Spacer(Modifier.height(10.dp))
            OutlinedTextField(value = filter, onValueChange = { filter = it },
                label = { Text("Filter functions") }, singleLine = true, modifier = Modifier.fillMaxWidth())

            if (ui.liveHistory.values.any { it.size >= 2 }) {
                Spacer(Modifier.height(10.dp))
                Card {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Text("LIVE", style = MaterialTheme.typography.labelSmall, color = OnMuted, modifier = Modifier.weight(1f))
                        TextButton(onClick = onShareCsv) { Icon(Icons.Filled.Share, null, Modifier.size(16.dp)); Spacer(Modifier.width(4.dp)); Text("CSV") }
                    }
                    com.owis.diag.ui.LineChart(ui.liveHistory, Modifier.fillMaxWidth().height(120.dp))
                    Spacer(Modifier.height(6.dp))
                    androidx.compose.foundation.layout.FlowRow(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                        ui.liveHistory.keys.forEachIndexed { i, name ->
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Box(Modifier.size(10.dp).clip(androidx.compose.foundation.shape.CircleShape)
                                    .background(com.owis.diag.ui.SeriesColors[i % com.owis.diag.ui.SeriesColors.size]))
                                Spacer(Modifier.width(4.dp))
                                Text(name, style = MaterialTheme.typography.labelSmall, color = OnMuted)
                            }
                        }
                    }
                }
            }
            if (ui.live.isNotEmpty()) {
                Spacer(Modifier.height(10.dp))
                LazyHorizontalGrid(rows = GridCells.Fixed(1), modifier = Modifier.height(78.dp),
                    horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    items(ui.live.entries.toList()) { (name, value) ->
                        Card(Modifier.width(150.dp)) {
                            Text(name, style = MaterialTheme.typography.labelSmall, color = OnMuted, maxLines = 1)
                            Text(if (value == value.toLong().toFloat()) value.toLong().toString() else String.format("%.1f", value),
                                style = MaterialTheme.typography.titleLarge, color = Accent)
                        }
                    }
                }
            }

            Spacer(Modifier.height(10.dp))
            val shown = fns.filter { filter.isBlank() || it.name.contains(filter, true) }
            LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(shown) { f ->
                    Surface(shape = RoundedCornerShape(12.dp), color = Surface1,
                        border = androidx.compose.foundation.BorderStroke(1.dp, Outline), modifier = Modifier.fillMaxWidth()) {
                        Row(Modifier.padding(start = 14.dp), verticalAlignment = Alignment.CenterVertically) {
                            Column(Modifier.weight(1f).padding(vertical = 12.dp)) {
                                Text(f.name, style = MaterialTheme.typography.bodyLarge, color = OnBg)
                                Spacer(Modifier.height(3.dp)); Chip(catLabel(f.cat), catColor(f.cat))
                            }
                            if (f.cat == "R") IconButton(onClick = { vm.live(module!!.index, intArrayOf(f.index), 30) }) {
                                Icon(Icons.Filled.Visibility, "watch", tint = Info)
                            }
                            IconButton(onClick = { if (isWrite(f.cat)) confirm = f else vm.runFunction(module!!.index, f.index) }, enabled = !ui.busy) {
                                Icon(Icons.Filled.PlayArrow, "run", tint = Accent)
                            }
                        }
                    }
                }
                item { Spacer(Modifier.height(20.dp)) }
            }
            if (ui.busy) OutlinedButton(onClick = { vm.stop() }, Modifier.fillMaxWidth()) {
                Icon(Icons.Filled.Stop, null); Spacer(Modifier.width(8.dp)); Text("Stop")
            }
        }
    }

    confirm?.let { f ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(f.name) },
            text = {
                Text(
                    (if (f.cat == "C") "This writes coding to the control unit (it unlocks with the unit's own seed/key). "
                    else "This runs a ${catLabel(f.cat)} on the control unit. ") +
                        "Make sure the car is safe before anything moves, and watch the log."
                )
            },
            confirmButton = { TextButton(onClick = { vm.runFunction(module!!.index, f.index); confirm = null }) { Text("Run") } },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } },
            containerColor = Surface2,
        )
    }
}
