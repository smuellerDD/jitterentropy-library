/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example

import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Scaffold
import androidx.compose.material3.PrimaryTabRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Tab
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.dynamicDarkColorScheme
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import kotlin.math.roundToInt
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

/**
 * Allocates one collector at start-up. The collector tab replaces it with a new
 * one: without flags, in FIPS or in NTG.1 mode, on the platform clock or on the
 * library's timer thread, and with the oversampling rate, memory size and hash
 * loop count its controls say. The output tab shows its status and 32 bytes of
 * its output, each on a button press. The record tab records the raw noise
 * data of a mode, with the same settings, for the entropy assessment.
 */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            JitterEntropyTheme {
                CollectorScreen()
            }
        }
    }
}

/** Material 3, in the wallpaper's colors where the system offers them. */
@Composable
fun JitterEntropyTheme(content: @Composable () -> Unit) {
    val dark = isSystemInDarkTheme()
    val colors = when {
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.S -> {
            val context = LocalContext.current
            if (dark) dynamicDarkColorScheme(context)
            else dynamicLightColorScheme(context)
        }
        dark -> darkColorScheme()
        else -> lightColorScheme()
    }
    MaterialTheme(colorScheme = colors, content = content)
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CollectorScreen(model: CollectorViewModel = viewModel()) {
    var tab by rememberSaveable { mutableIntStateOf(0) }

    Scaffold(
        topBar = { TopAppBar(title = { Text(stringResource(R.string.app_name)) }) }
    ) { insets ->
        Column(modifier = Modifier.fillMaxSize().padding(insets)) {
            PrimaryTabRow(selectedTabIndex = tab) {
                Tab(
                    selected = tab == 0,
                    onClick = { tab = 0 },
                    text = { Text(stringResource(R.string.tab_collector)) }
                )
                Tab(
                    selected = tab == 1,
                    onClick = { tab = 1 },
                    text = { Text(stringResource(R.string.tab_output)) }
                )
                Tab(
                    selected = tab == 2,
                    onClick = { tab = 2 },
                    text = { Text(stringResource(R.string.tab_record)) }
                )
            }
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(horizontal = 16.dp, vertical = 8.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Text(
                    stringResource(R.string.version, JitterEntropy.version),
                    style = MaterialTheme.typography.titleMedium
                )
                Text(
                    model.state,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
                when (tab) {
                    0 -> Controls(model, Modifier.verticalScroll(rememberScrollState()))
                    1 -> OutputPane(model)
                    else -> RecordPane(model)
                }
            }
        }
    }
}

/** The settings of the next collector and the buttons that allocate it. */
@Composable
private fun Controls(model: CollectorViewModel, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier,
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            stringResource(R.string.new_collector),
            style = MaterialTheme.typography.labelLarge,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        // The row is the switch, so that the label toggles it as well and
        // TalkBack announces the two as one control.
        Row(
            modifier = Modifier.toggleable(
                value = model.timerThread,
                enabled = !model.allocating,
                role = Role.Switch,
                onValueChange = { model.timerThread = it }
            ),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Switch(
                checked = model.timerThread,
                onCheckedChange = null,
                enabled = !model.allocating
            )
            Text(stringResource(R.string.timer_thread))
        }
        Setting(
            stringResource(R.string.osr, model.osr),
            model.osr, JitterEntropy.MIN_OSR, JitterEntropy.MAX_OSR,
            !model.allocating
        ) { model.osr = it }
        Setting(
            stringResource(R.string.memory_size,
                           JitterEntropy.memSizeLabel(model.memSize)),
            model.memSize, 0, JitterEntropy.MAX_MEMSIZE, !model.allocating
        ) { model.memSize = it }
        Setting(
            stringResource(R.string.hash_loop,
                           JitterEntropy.hashLoopLabel(model.hashLoop)),
            model.hashLoop, 0, JitterEntropy.MAX_HASHLOOP, !model.allocating
        ) { model.hashLoop = it }
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            for ((mode, label) in listOf(
                JitterEntropy.Mode.DEFAULT to R.string.mode_default,
                JitterEntropy.Mode.FIPS to R.string.mode_fips,
                JitterEntropy.Mode.NTG1 to R.string.mode_ntg1
            )) {
                // NTG.1 forbids the timer thread; the library refuses it.
                val refused = mode == JitterEntropy.Mode.NTG1 &&
                    model.timerThread
                OutlinedButton(
                    onClick = { model.allocate(mode) },
                    // Behind a recording on the worker thread, it would wait minutes.
                    enabled = !model.allocating && !model.recording && !refused,
                    modifier = Modifier.weight(1f)
                ) {
                    Text(stringResource(label))
                }
            }
        }
        if (model.timerThread) {
            Text(
                stringResource(R.string.timer_thread_note),
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

/** The buttons that use the collector, and what they returned. */
@Composable
private fun OutputPane(model: CollectorViewModel) {
    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            FilledTonalButton(
                onClick = model::showStatus,
                enabled = model.ready && !model.recording,
                modifier = Modifier.weight(1f)
            ) {
                Text(stringResource(R.string.show_status))
            }
            Button(
                onClick = model::generate,
                enabled = model.ready && !model.recording,
                modifier = Modifier.weight(1f)
            ) {
                Text(stringResource(R.string.generate))
            }
        }
        Output(model.output, Modifier.fillMaxHeight())
    }
}

/**
 * The buttons that record the raw noise data of a mode, and how far the
 * recording is. The screen stays on while it runs: a phone that locks
 * suspends the app, and the timing of what runs then is not the app's.
 */
@Composable
private fun RecordPane(model: CollectorViewModel) {
    /*
     * Internal storage, which adb reads through run-as, as the app is a debug
     * build; see recording_library/README.md. Not the external files
     * directory: on Android 8 its emulated file system failed to create a
     * file in a directory holding a few thousand, as an NTG.1 recording
     * does, at a different count each time.
     */
    val base = LocalContext.current.filesDir
    val view = LocalView.current

    DisposableEffect(model.recording) {
        view.keepScreenOn = model.recording
        onDispose { view.keepScreenOn = false }
    }

    Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
        Text(
            stringResource(R.string.record_note, base.resolve(Recorder.DIR).path),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            for ((mode, label) in listOf(
                JitterEntropy.Mode.DEFAULT to R.string.mode_default,
                JitterEntropy.Mode.FIPS to R.string.mode_fips,
                JitterEntropy.Mode.NTG1 to R.string.mode_ntg1
            )) {
                // NTG.1 forbids the timer thread; the library refuses it.
                val refused = mode == JitterEntropy.Mode.NTG1 &&
                    model.timerThread
                OutlinedButton(
                    onClick = { model.record(mode, base) },
                    enabled = !model.recording && !model.allocating && !refused,
                    modifier = Modifier.weight(1f)
                ) {
                    Text(stringResource(label))
                }
            }
        }
        if (model.recording) {
            LinearProgressIndicator(
                progress = { model.recordProgress },
                modifier = Modifier.fillMaxWidth()
            )
            FilledTonalButton(
                onClick = model::cancelRecord,
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(stringResource(R.string.cancel))
            }
        }
        Output(model.recordState, Modifier.fillMaxHeight())
    }
}

/** A labelled slider over the integers from [min] to [max]. */
@Composable
private fun Setting(
    label: String,
    value: Int,
    min: Int,
    max: Int,
    enabled: Boolean,
    onChange: (Int) -> Unit
) {
    Column {
        Text(label)
        Slider(
            value = value.toFloat(),
            onValueChange = { onChange(it.roundToInt()) },
            valueRange = min.toFloat()..max.toFloat(),
            steps = max - min - 1,
            enabled = enabled
        )
    }
}

/** The status document or the output, scrolling within the space it is given. */
@Composable
private fun Output(text: String, modifier: Modifier = Modifier) {
    if (text.isEmpty())
        return

    OutlinedCard(modifier = modifier.fillMaxWidth()) {
        SelectionContainer {
            Text(
                text,
                fontFamily = FontFamily.Monospace,
                style = MaterialTheme.typography.bodySmall,
                modifier = Modifier
                    .verticalScroll(rememberScrollState())
                    .padding(12.dp)
            )
        }
    }
}
