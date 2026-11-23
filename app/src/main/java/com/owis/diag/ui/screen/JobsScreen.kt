package com.owis.diag.ui.screen

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import com.owis.diag.UiState
import com.owis.diag.data.Job
import com.owis.diag.ui.Card
import com.owis.diag.ui.theme.*

@Composable
fun JobsScreen(ui: UiState, onRun: (String) -> Unit) {
    var confirm by remember { mutableStateOf<Job?>(null) }
    Column(Modifier.fillMaxSize()) {
        Text("Guided jobs", style = MaterialTheme.typography.titleLarge, color = OnBg, modifier = Modifier.padding(16.dp))
        if (ui.jobs.isEmpty()) {
            Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                Text(if (ui.connected) "No jobs for this vehicle" else "Connect to a car first", color = OnMuted)
            }
        } else LazyColumn(Modifier.fillMaxSize().padding(horizontal = 16.dp), verticalArrangement = Arrangement.spacedBy(10.dp)) {
            items(ui.jobs) { j ->
                Card {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Filled.Build, null, tint = Accent)
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text(j.title, style = MaterialTheme.typography.titleMedium, color = OnBg)
                            if (j.note.isNotEmpty()) Text(j.note, style = MaterialTheme.typography.bodyMedium, color = OnMuted)
                        }
                        FilledTonalIconButton(onClick = { confirm = j }, enabled = !ui.busy) {
                            Icon(Icons.Filled.PlayArrow, "run")
                        }
                    }
                }
            }
            item { Spacer(Modifier.height(24.dp)) }
        }
    }
    confirm?.let { j ->
        AlertDialog(
            onDismissRequest = { confirm = null },
            title = { Text(j.title) },
            text = { Text("Run this job on the car now? Follow the on-screen log and keep the ignition on. " +
                "Make sure the car is safe (handbrake, clear of people) before anything moves.") },
            confirmButton = { TextButton(onClick = { onRun(j.id); confirm = null }) { Text("Run") } },
            dismissButton = { TextButton(onClick = { confirm = null }) { Text("Cancel") } },
            containerColor = Surface2,
        )
    }
}
