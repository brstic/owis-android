package com.owis.diag.ui.screen

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.dp
import com.owis.diag.UiState
import com.owis.diag.ui.theme.*

@Composable
fun LogScreen(ui: UiState, onStop: () -> Unit) {
    val state = rememberLazyListState()
    LaunchedEffect(ui.log.size) { if (ui.log.isNotEmpty()) state.animateScrollToItem(ui.log.size - 1) }
    Column(Modifier.fillMaxSize()) {
        Row(Modifier.fillMaxWidth().padding(16.dp)) {
            Text("Session log", style = MaterialTheme.typography.titleLarge, color = OnBg, modifier = Modifier.weight(1f))
            if (ui.busy) OutlinedButton(onClick = onStop) { Icon(Icons.Filled.Stop, null, Modifier.size(18.dp)); Spacer(Modifier.width(6.dp)); Text("Stop") }
        }
        LazyColumn(state = state, modifier = Modifier.fillMaxSize().padding(horizontal = 16.dp)) {
            items(ui.log) { line ->
                Text(line, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall,
                    color = colorFor(line), modifier = Modifier.padding(vertical = 1.dp))
            }
            item { Spacer(Modifier.height(24.dp)) }
        }
    }
}

private fun colorFor(line: String) = when {
    line.contains("[OK]") || line.contains("[UNLOCKED]") || line.contains("[DONE]") -> Accent
    line.contains("[ERROR]") || line.contains("[FAIL") || line.contains("[LOCKED]") -> Danger
    line.contains("[") -> Warn
    line.startsWith(">") -> Info
    else -> OnMuted
}
