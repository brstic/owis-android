package com.owis.diag.ui.screen

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.owis.diag.MainViewModel
import com.owis.diag.UiState
import com.owis.diag.data.Fn
import com.owis.diag.data.ModuleRef
import com.owis.diag.ui.Card
import com.owis.diag.ui.theme.*

@Composable
fun LiveScreen(vm: MainViewModel, ui: UiState) {
    var modules by remember { mutableStateOf<List<ModuleRef>>(emptyList()) }
    var module by remember { mutableStateOf<ModuleRef?>(null) }
    var fns by remember { mutableStateOf<List<Fn>>(emptyList()) }
    var picking by remember { mutableStateOf(false) }

    LaunchedEffect(ui.vehicle) { if (ui.vehicle != null) vm.modules { modules = it } }

    Column(Modifier.fillMaxSize().padding(16.dp)) {
        Text("Live data", style = MaterialTheme.typography.titleLarge, color = OnBg)
        Spacer(Modifier.height(12.dp))

        // module picker
        Box {
            OutlinedButton(onClick = { picking = true }, Modifier.fillMaxWidth()) {
                Text(module?.name ?: "Choose a control unit", Modifier.weight(1f), color = OnBg)
                Icon(Icons.Filled.ExpandMore, null)
            }
            DropdownMenu(expanded = picking, onDismissRequest = { picking = false }) {
                modules.forEach { m ->
                    DropdownMenuItem(text = { Text(m.name) }, onClick = {
                        module = m; picking = false
                        vm.functions(m.index, "") { list -> fns = list.filter { it.cat == 'R'.toString() || it.name.isNotEmpty() } }
                    })
                }
            }
        }
        Spacer(Modifier.height(12.dp))

        // live value tiles
        if (ui.live.isNotEmpty()) {
            LazyVerticalGrid(columns = GridCells.Fixed(2), verticalArrangement = Arrangement.spacedBy(10.dp),
                horizontalArrangement = Arrangement.spacedBy(10.dp), modifier = Modifier.weight(1f, fill = false)) {
                items(ui.live.entries.toList()) { (name, value) ->
                    Card {
                        Text(name, style = MaterialTheme.typography.bodyMedium, color = OnMuted, maxLines = 2)
                        Spacer(Modifier.height(6.dp))
                        Text(fmt(value), style = MaterialTheme.typography.headlineMedium, color = Accent)
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
        }

        // function list to start a live read
        if (module != null) {
            Text("READ FUNCTIONS", style = MaterialTheme.typography.labelSmall, color = OnMuted)
            Spacer(Modifier.height(6.dp))
            LazyColumn(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                items(fns) { f ->
                    Surface(onClick = { vm.live(module!!.index, intArrayOf(f.index), 20) },
                        shape = androidx.compose.foundation.shape.RoundedCornerShape(12.dp), color = Surface1,
                        border = androidx.compose.foundation.BorderStroke(1.dp, Outline), modifier = Modifier.fillMaxWidth()) {
                        Text(f.name, Modifier.padding(14.dp), style = MaterialTheme.typography.bodyLarge, color = OnBg)
                    }
                }
            }
        }

        if (ui.busy) {
            Spacer(Modifier.height(8.dp))
            OutlinedButton(onClick = { vm.stop() }, Modifier.fillMaxWidth()) {
                Icon(Icons.Filled.Stop, null); Spacer(Modifier.width(8.dp)); Text("Stop")
            }
        }
    }
}

private fun fmt(v: Float): String = if (v == v.toLong().toFloat()) v.toLong().toString() else String.format("%.1f", v)
