package com.owis.diag.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.owis.diag.ui.theme.*

@Composable
fun Card(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(18.dp),
        color = Surface1,
        border = BorderStroke(1.dp, Outline),
    ) { Column(Modifier.padding(16.dp), content = content) }
}

@Composable
fun Chip(text: String, color: Color = Info) {
    Box(
        Modifier.clip(RoundedCornerShape(8.dp)).background(color.copy(alpha = 0.16f)).padding(horizontal = 8.dp, vertical = 3.dp)
    ) { Text(text, style = MaterialTheme.typography.labelSmall, color = color) }
}

@Composable
fun StatusDot(ok: Boolean, warn: Boolean = false) {
    val c = if (warn) Warn else if (ok) Accent else OnMuted
    Box(Modifier.size(10.dp).clip(CircleShape).background(c))
}

@Composable
fun SectionTitle(text: String, modifier: Modifier = Modifier) =
    Text(text, style = MaterialTheme.typography.labelSmall, color = OnMuted, modifier = modifier)

fun statusColor(status: String): Color = when {
    status.contains("active now", true) || status.contains("test failed", true) -> Danger
    status.contains("stored", true) || status.contains("warning", true) -> Warn
    else -> OnMuted
}

@Composable
fun Mono(text: String, color: Color = OnMuted, modifier: Modifier = Modifier) =
    Text(text, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall, color = color,
        modifier = modifier, overflow = TextOverflow.Ellipsis)
