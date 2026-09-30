/*
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example

import android.system.Os
import java.io.File
import java.util.Locale

/**
 * Records the raw noise data of the SP800-90B and NTG.1 assessments in the
 * app, as the recording scripts of tests/raw-entropy/recording_userspace do
 * on a host: the same sets, the same file names, one directory the validation
 * scripts read as it is. See tests/raw-entropy/recording_library/README.md.
 *
 * The recording goes to [DIR].partial beside [DIR] and replaces [DIR] only
 * once it is complete, so that a cancelled or failed one is never taken for
 * data.
 *
 * Calls into the library: run it on the thread every other call runs on.
 */
class Recorder(private val base: File) {
    /** The noise source of a set, the JENT_RECORD_* value of jitterentropy-record.h. */
    enum class Source(val value: Int) {
        COMMON(0),
        HASHLOOP(1),
        MEMACCESS(2)
    }

    /**
     * [repeats] files named [name]-0001.data onwards, of [rounds] time deltas
     * each, as jitterentropy-hashtime <rounds> <repeats> <name> writes them.
     */
    data class DataSet(val name: String, val source: Source, val rounds: Int, val repeats: Int)

    /** A recording that failed or was cancelled; nothing of it is kept. */
    class Failure(message: String) : Exception(message)

    /** What a complete recording left in [directory], and its health test failures per set. */
    class Result(val directory: File, val health: Map<String, String>)

    /**
     * Records the sets of [config]'s mode with its flags and oversampling
     * rate. [cancelled] is asked between files, [progress] told the share of
     * the samples recorded so far and the file at hand. [description] heads
     * the recording.txt left beside the data.
     */
    fun record(
        config: JitterEntropy.Config,
        description: String,
        cancelled: () -> Boolean,
        progress: (Double, String) -> Unit
    ): Result {
        val sets = sets(config.mode)
        val total = sets.sumOf { it.rounds.toLong() * it.repeats }
        val partial = File(base, "$DIR.partial")
        val health = linkedMapOf<String, String>()
        var done = 0L

        partial.deleteRecursively()
        if (!partial.mkdirs())
            throw Failure("cannot create $partial")

        try {
            for (set in sets) {
                var failures = 0
                for (i in 1..set.repeats) {
                    if (cancelled())
                        throw Failure("recording cancelled")

                    /* Locale.ROOT: ASCII digits, as the validation scripts expect. */
                    val file = File(partial, "%s-%04d.data".format(Locale.ROOT, set.name, i))
                    progress(done.toDouble() / total, file.name)

                    val out = IntArray(2)
                    val ret = nativeRecord(file.path, set.rounds, config.osr,
                                           config.flags, set.source.value, out)
                    if (ret != 0)
                        throw Failure("recording ${file.name} failed ($config): " +
                                      describe(ret, out[1]))
                    failures = failures or out[0]
                    done += set.rounds
                }
                health[set.name] = if (failures == 0) "none" else nativeHealthNames(failures)
            }

            File(partial, "recording.txt").writeText(buildString {
                appendLine(description)
                appendLine("Configuration: $config")
                appendLine("Flags: 0x%08x".format(config.flags))
                for (set in sets)
                    appendLine("${set.name}: ${set.repeats} x ${set.rounds} " +
                               "time deltas, health test failures: ${health[set.name]}")
            })
        } catch (e: Exception) {
            partial.deleteRecursively()
            throw e
        }

        val directory = File(base, DIR)
        directory.deleteRecursively()
        if (!partial.renameTo(directory))
            throw Failure("cannot rename $partial to $directory")
        progress(1.0, "")

        return Result(directory, health)
    }

    companion object {
        /** The directory the validation scripts read: tests/raw-entropy/results-measurements. */
        const val DIR = "results-measurements"

        /* NUM_EVENTS, NUM_EVENTS_RESTART and NUM_RESTART of invoke_testing_helper.sh */
        private const val EVENTS = 1_000_000
        private const val EVENTS_RESTART = 1000
        private const val RESTARTS = 1000

        /**
         * What invoke_testing.sh (default), invoke_testing_fips.sh and
         * invoke_testing_ntg1.sh record, in their order.
         */
        fun sets(mode: JitterEntropy.Mode): List<DataSet> {
            val common = listOf(
                DataSet("jent-raw-noise", Source.COMMON, EVENTS, 1),
                DataSet("jent-raw-noise-restart", Source.COMMON, EVENTS_RESTART, RESTARTS)
            )
            if (mode != JitterEntropy.Mode.NTG1)
                return common

            return common + listOf(
                DataSet("jent-raw-noise_hashloop", Source.HASHLOOP, EVENTS, 1),
                DataSet("jent-raw-noise_memaccloop", Source.MEMACCESS, EVENTS, 1),
                DataSet("jent-raw-noise-hashloop-restart", Source.HASHLOOP,
                    EVENTS_RESTART, RESTARTS),
                DataSet("jent-raw-noise-memaccloop-restart", Source.MEMACCESS,
                    EVENTS_RESTART, RESTARTS)
            )
        }

        /** Names a JENT_RECORD_* code of jent_record_raw(), with the errno of EIO. */
        private fun describe(code: Int, errno: Int): String = when (code) {
            1 -> "invalid noise source or sample count"
            2 -> "no memory for the samples"
            3 -> "no collector: the self tests failed, the flags were refused, " +
                "or the memory could not be allocated or locked"
            4 -> "the file could not be written: ${Os.strerror(errno)} ($errno)"
            else -> "unknown error ($code)"
        }

        init {
            System.loadLibrary("jitterentropy_jni")
        }

        /* @JvmStatic: the JNI names in jitterentropy-jni.c use this class. */
        @JvmStatic private external fun nativeRecord(
            path: String, rounds: Int, osr: Int, flags: Int, source: Int,
            out: IntArray
        ): Int
        @JvmStatic private external fun nativeHealthNames(health: Int): String
    }
}
