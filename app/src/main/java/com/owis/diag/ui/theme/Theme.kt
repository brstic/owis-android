package com.owis.diag.ui.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable

private val OwisColors = darkColorScheme(
    primary = Accent,
    onPrimary = Bg,
    primaryContainer = AccentDim,
    onPrimaryContainer = Accent,
    secondary = Info,
    onSecondary = Bg,
    background = Bg,
    onBackground = OnBg,
    surface = Surface1,
    onSurface = OnBg,
    surfaceVariant = Surface2,
    onSurfaceVariant = OnMuted,
    outline = Outline,
    error = Danger,
    onError = Bg,
)

@Composable
fun OwisTheme(content: @Composable () -> Unit) {
    MaterialTheme(colorScheme = OwisColors, typography = OwisType, content = content)
}
