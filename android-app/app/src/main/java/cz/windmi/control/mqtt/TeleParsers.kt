package cz.windmi.control.mqtt

import cz.windmi.control.screen.RegMode

internal const val TELE_NA = "___"

internal fun normalizeRegMode(value: String): RegMode {
    return when (value.trim().lowercase()) {
        "room", "auto", "pokoj" -> RegMode.Room
        "equitherm", "ekv", "ekviterm", "ekvitermá" -> RegMode.Equitherm
        "water", "manual", "voda", "rucni", "vystupni" -> RegMode.Water
        else -> RegMode.Room
    }
}

internal fun parseOnOff(value: String): Boolean {
    val s = value.trim().uppercase()
    if (s.isEmpty() || s == "OFF" || s == "0" || s == "FALSE" || s == "STOP" ||
        s == TELE_NA || s == "---"
    ) {
        return false
    }
    if (s == "ON" || s == "1" || s == "TRUE" || s == "START" || s == "YES") {
        return true
    }
    val n = s.toFloatOrNull()
    return n != null && n > 0f
}

internal fun parseTemp(value: String): Float? {
    val s = value.trim()
    if (s.isEmpty() || s == TELE_NA || s == "---" || s.equals("off", ignoreCase = true)) {
        return null
    }
    return s.toFloatOrNull()
}
