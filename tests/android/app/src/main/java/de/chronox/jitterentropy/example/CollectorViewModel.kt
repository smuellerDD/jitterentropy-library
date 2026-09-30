/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example

import android.os.Build
import android.util.Log
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import java.io.File
import java.io.IOException
import java.util.Date
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.launch
import kotlinx.coroutines.plus
import kotlinx.coroutines.withContext

private const val TAG = "JitterEntropyExample"

/**
 * Owns the collector, so that it outlives the activity being recreated - a
 * rotation would otherwise rerun the power-on tests.
 */
class CollectorViewModel : ViewModel() {
    var state by mutableStateOf("")
        private set
    var output by mutableStateOf("")
        private set
    var ready by mutableStateOf(false)
        private set
    var allocating by mutableStateOf(false)
        private set

    /** Whether the next collector uses the timer thread; see JitterEntropy.Config. */
    var timerThread by mutableStateOf(false)

    /** The initial oversampling rate, memory size and hash loop count of the next collector. */
    var osr by mutableStateOf(JitterEntropy.MIN_OSR)
    var memSize by mutableStateOf(0)
    var hashLoop by mutableStateOf(0)

    /** A raw noise recording running, its share done, and what it reported last. */
    var recording by mutableStateOf(false)
        private set
    var recordProgress by mutableStateOf(0f)
        private set
    var recordState by mutableStateOf("")
        private set
    private val cancelRecording = AtomicBoolean(false)

    /*
     * One thread for every call into the library: the power-on tests and
     * collection take long enough to freeze the UI, and a collector belongs
     * to one thread at a time.
     */
    private val worker = Executors.newSingleThreadExecutor().asCoroutineDispatcher()
    private var collector: JitterEntropy? = null
    private var cleared = false

    /**
     * Takes over [built], on the worker thread that built it - unless the
     * model was cleared in the meantime, in which case nobody is left to
     * close it and it is closed here.
     */
    @Synchronized
    private fun adopt(built: JitterEntropy) {
        if (cleared) built.close() else collector = built
    }

    /** Hands the current collector over and leaves none behind. */
    @Synchronized
    private fun detach(clearing: Boolean = false): JitterEntropy? {
        if (clearing) cleared = true
        val c = collector
        collector = null
        return c
    }

    init {
        allocate(JitterEntropy.Mode.DEFAULT)
    }

    /** Replaces the collector with a new one in [mode] and the settings above. */
    fun allocate(mode: JitterEntropy.Mode) {
        val config = JitterEntropy.Config(mode, timerThread, osr, memSize, hashLoop)

        // Not through the coroutine, which a cleared model cancels before it
        // ran; queued ahead of the allocation below all the same.
        val old = detach()
        worker.executor.execute { old?.close() }

        state = "Allocating the entropy collector ($config)…"
        output = ""
        ready = false
        allocating = true
        viewModelScope.launch {
            try {
                /*
                 * The collector is taken over on the worker thread that built
                 * it, and under NonCancellable.
                 *
                 * withContext() throws CancellationException on resume when
                 * the scope was cancelled while its block ran, and the value
                 * the block produced is discarded with it - so a plain
                 * "collector = withContext(worker) { … }" loses the collector
                 * whenever the model is cleared during an allocation. And
                 * ViewModel.clear() cancels viewModelScope before calling
                 * onCleared(), so onCleared() would find no collector, close
                 * nothing, and then shut the worker down: the struct
                 * rand_data, its memory region and, in a compliance mode, its
                 * locked pages are leaked, JitterEntropy having no finalizer.
                 * Backing out of the activity while osr 20 / 512 MB / hash
                 * loop 128 is being built is all it takes.
                 *
                 * The close path above already takes the same care.
                 */
                withContext(worker + NonCancellable) {
                    adopt(JitterEntropy(config))
                }
                Log.i(TAG, "collector allocated ($config)")
                state = "Entropy collector allocated ($config)"
                ready = true
            } catch (e: JitterEntropy.Failure) {
                Log.e(TAG, e.message!!)
                state = e.message!!
            } finally {
                allocating = false
            }
        }
    }

    fun showStatus() {
        val c = collector ?: return
        viewModelScope.launch {
            val status = withContext(worker) { c.status() }
            Log.i(TAG, "status: $status")
            output = status ?: "jent_status failed"
        }
    }

    fun generate() {
        val c = collector ?: return
        viewModelScope.launch {
            output = try {
                val hex = withContext(worker) {
                    c.read(32).joinToString("") { "%02x".format(it.toInt() and 0xff) }
                }
                Log.i(TAG, "32 bytes: $hex")
                hex
            } catch (e: JitterEntropy.Failure) {
                Log.e(TAG, e.message!!)
                e.message!!
            }
        }
    }

    /**
     * Records the raw noise data of [mode] with the settings above into
     * [base]/results-measurements; see [Recorder].
     */
    fun record(mode: JitterEntropy.Mode, base: File) {
        val config = JitterEntropy.Config(mode, timerThread, osr, memSize, hashLoop)

        cancelRecording.set(false)
        recording = true
        recordProgress = 0f
        recordState = "Recording ($config)…"
        viewModelScope.launch {
            try {
                val result = withContext(worker) {
                    // Compose state may be written from the worker thread.
                    Recorder(base).record(config, description(),
                                          { cancelRecording.get() }) { share, file ->
                        recordProgress = share.toFloat()
                        if (file.isNotEmpty())
                            recordState = "Recording ($config): $file"
                    }
                }
                val health = result.health.entries.joinToString("\n") {
                    "${it.key}: ${it.value}"
                }
                recordState = "Recorded ($config) into ${result.directory}\n" +
                    "Health test failures:\n$health"
                Log.i(TAG, recordState)
            } catch (e: Recorder.Failure) {
                Log.e(TAG, e.message!!)
                recordState = e.message!!
            } catch (e: IOException) {
                Log.e(TAG, "recording failed", e)
                recordState = "Recording failed ($config): $e"
            } finally {
                recording = false
            }
        }
    }

    /** Stops the recording before its next file; it records nothing then. */
    fun cancelRecord() {
        cancelRecording.set(true)
    }

    /** What recorded the data, for the recording.txt beside it. */
    private fun description(): String {
        val soc = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S)
            " (${Build.SOC_MANUFACTURER} ${Build.SOC_MODEL})" else ""

        return "Recorded by the Jitter RNG ${JitterEntropy.version} example app " +
            "on ${Date()}\n" +
            "Device: ${Build.MANUFACTURER} ${Build.MODEL}$soc, " +
            "${Build.SUPPORTED_ABIS.first()}\n" +
            "Android ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT}), " +
            "build ${Build.FINGERPRINT}"
    }

    override fun onCleared() {
        // A recording runs on to its next file, where this stops it.
        cancelRecording.set(true)
        /*
         * Queued behind whatever the worker is still running - including an
         * allocation whose scope clear() has already cancelled: it runs to the
         * end under NonCancellable and either hands its collector over here or,
         * seeing that this ran first, closes it itself. close() shuts the
         * executor down but lets what is already queued run.
         */
        val c = detach(clearing = true)
        worker.executor.execute { c?.close() }
        worker.close()
    }
}
