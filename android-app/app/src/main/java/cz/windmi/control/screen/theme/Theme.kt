package cz.windmi.control.screen.theme

import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val scheme = darkColorScheme(
    primary = Room,
    onPrimary = Color.White,
    secondary = Water,
    background = Bg,
    surface = Panel,
    onBackground = TextPrimary,
    onSurface = TextPrimary,
    error = Fault,
)

@Composable
fun WindmiTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = scheme,
        content = content,
    )
}
