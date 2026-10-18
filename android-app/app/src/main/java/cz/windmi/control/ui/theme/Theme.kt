package cz.windmi.control.ui.theme

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
    onBackground = Text,
    onSurface = Text,
    error = Fault,
)

@Composable
fun WindmiTheme(content: @Composable () -> Unit) {
    MaterialTheme(
        colorScheme = scheme,
        content = content,
    )
}
