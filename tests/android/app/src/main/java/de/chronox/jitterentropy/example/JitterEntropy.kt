/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

package de.chronox.jitterentropy.example

/**
 * One Jitter RNG entropy collector with the flags and the oversampling rate of
 * [config]; the constructor runs the power-on tests first.
 *
 * A collector belongs to one thread at a time, so the methods are
 * synchronized. Collecting takes milliseconds: keep it off the main thread.
 */
class JitterEntropy(private val config: Config = Config(Mode.DEFAULT)) :
    AutoCloseable {
    /**
     * The mode flags of jitterentropy.h. Both compliance modes imply
     * JENT_FORCE_SECURE_MEM, so they fail with EMEM where the state cannot
     * be locked.
     */
    enum class Mode(val flags: Int, private val label: String) {
        DEFAULT(0, "default"),
        FIPS(1 shl 5, "FIPS"),        /* JENT_FORCE_FIPS */
        NTG1(1 shl 6, "NTG.1");       /* JENT_NTG1 */

        override fun toString() = label
    }

    /**
     * A [mode], whether time stamps come from the library's timer thread
     * rather than the platform clock (JENT_FORCE_INTERNAL_TIMER), and the
     * initial oversampling rate, memory size and hash loop count.
     *
     * A health test failure raises the oversampling rate and the hash loop
     * count of the replacement collector. It does not raise the memory size
     * once one has been chosen here: jent_update_memsize() applies its
     * increment only to a size the library derived itself, so leaving
     * [memSize] at 0 (automatic) is what lets the recovery grow it.
     *
     * [memSize] is the JENT_MAX_MEMSIZE field: 0 derives the size from the
     * CPU caches, n is 2^(n + 9) bytes. [hashLoop] is the JENT_HASHLOOP
     * field: 0 is the library's default count, n is 2^(n - 1) loops.
     *
     * The choice is per collector. NTG.1 forbids the timer thread.
     */
    data class Config(
        val mode: Mode,
        val timerThread: Boolean = false,
        val osr: Int = MIN_OSR,
        val memSize: Int = 0,
        val hashLoop: Int = 0
    ) {
        val flags: Int
            get() = mode.flags or
                (if (timerThread) FORCE_INTERNAL_TIMER else 0) or
                (memSize shl MEMSIZE_SHIFT) or
                (hashLoop shl HASHLOOP_SHIFT)

        override fun toString() =
            (if (timerThread) "$mode, timer thread" else "$mode") +
                ", osr $osr, memory ${memSizeLabel(memSize)}, " +
                "hash loop ${hashLoopLabel(hashLoop)}"
    }

    /** A failed call into the library, carrying the code it returned. */
    class Failure : Exception {
        constructor(call: String, config: Config, code: Int) :
            super("$call failed ($config): ${describe(code)} ($code)")

        /** For a call that reports no code of its own. */
        constructor(call: String, config: Config, why: String) :
            super("$call failed ($config): $why")
    }

    private var handle = 0L

    init {
        val ret = nativeInit(config.osr, config.flags)
        if (ret != 0)
            throw Failure("jent_entropy_init_ex", config, ret)

        handle = nativeAlloc(config.osr, config.flags)
        if (handle == 0L) {
            /*
             * The allocation answers with a null pointer and no code of its
             * own, so there is nothing to describe: this used to report
             * "unknown error (0)", which says neither what failed nor what to
             * change. Both settings that reach it are on the screen.
             */
            val why = if (config.mode == Mode.DEFAULT)
                "no collector was returned. The memory size " +
                    "(${memSizeLabel(config.memSize)}) may be more than this " +
                    "process can get; try a smaller one."
            else
                "no collector was returned. $config needs its memory " +
                    "(${memSizeLabel(config.memSize)}) locked into RAM, which " +
                    "Android grants only within RLIMIT_MEMLOCK; try a smaller " +
                    "memory size or the default mode."

            throw Failure("jent_entropy_collector_alloc", config, why)
        }
    }

    /** Returns [len] bytes from the collector. */
    @Synchronized
    fun read(len: Int): ByteArray {
        val out = ByteArray(len)
        val ret = nativeRead(checkedHandle(), out)
        if (ret != 0)
            throw Failure("jent_read_entropy_safe", config, ret)
        return out
    }

    /** The JSON status document of this collector. */
    @Synchronized
    fun status(): String? = nativeStatus(checkedHandle())

    @Synchronized
    override fun close() {
        nativeFree(handle)
        handle = 0L
    }

    private fun checkedHandle(): Long {
        check(handle != 0L) { "collector closed" }
        return handle
    }

    companion object {
        /* JENT_FORCE_INTERNAL_TIMER */
        private const val FORCE_INTERNAL_TIMER = 1 shl 3

        /* JENT_MIN_OSR and JENT_MAX_OSR, which jitterentropy.h does not export. */
        const val MIN_OSR = 3
        const val MAX_OSR = 20

        /* JENT_FLAGS_TO_MEMSIZE_SHIFT and JENT_MAX_MEMSIZE_MAX (512 MB). */
        private const val MEMSIZE_SHIFT = 27
        const val MAX_MEMSIZE = 20

        /*
         * JENT_FLAGS_TO_HASHLOOP_SHIFT, and the JENT_MAX_HASHLOOP field value
         * (128 loops).
         */
        private const val HASHLOOP_SHIFT = 23
        const val MAX_HASHLOOP = 8

        /** A JENT_MAX_MEMSIZE field value as a size. */
        fun memSizeLabel(memSize: Int): String = when {
            memSize == 0 -> "automatic"
            memSize < 11 -> "${1 shl (memSize - 1)} kB"
            else -> "${1 shl (memSize - 11)} MB"
        }

        /** A JENT_HASHLOOP field value as a loop count. */
        fun hashLoopLabel(hashLoop: Int): String =
            if (hashLoop == 0) "default" else "${1 shl (hashLoop - 1)}"

        init {
            System.loadLibrary("jitterentropy_jni")
        }

        /** The library version as "major.minor.patchlevel". */
        val version: String
            get() {
                val v = nativeVersion()
                return "${v / 1000000}.${v / 10000 % 100}.${v / 100 % 100}"
            }

        /** Names a return code of the library. */
        fun describe(code: Int): String = when (code) {
            /* jent_entropy_init_ex() */
            1 -> "ENOTIME: timer service not available"
            2 -> "ECOARSETIME: timer too coarse"
            3 -> "ENOMONOTONIC: timer not monotonic"
            6 -> "EMINVARVAR: timer variations too small"
            8 -> "ESTUCK: too many stuck results"
            9 -> "EHEALTH: health test failed"
            10 -> "ERCT: RCT failed"
            11 -> "EHASH: hash self test failed"
            12 -> "EMEM: cannot allocate memory"
            13 -> "EGCD: GCD self test failed"
            /* jent_read_entropy() */
            -1 -> "JENT_ERR_EINVAL: invalid collector"
            -2 -> "JENT_ERR_RCT: intermittent RCT failure"
            -3 -> "JENT_ERR_APT: intermittent APT failure"
            -4 -> "JENT_ERR_NOTIME: timer cannot be initialized"
            -5 -> "JENT_ERR_LAG: intermittent lag predictor failure"
            -6 -> "JENT_ERR_RCT_PERMANENT: permanent RCT failure"
            -7 -> "JENT_ERR_APT_PERMANENT: permanent APT failure"
            -8 -> "JENT_ERR_LAG_PERMANENT: permanent lag predictor failure"
            -9 -> "JENT_ERR_RCT_MEM: intermittent RCT with memory failure"
            -10 -> "JENT_ERR_RCT_MEM_PERMANENT: permanent RCT with memory failure"
            -11 -> "JENT_ERR_SELFTEST: self test failed"
            else -> "unknown error"
        }

        /* @JvmStatic: the JNI names in jitterentropy-jni.c use this class. */
        @JvmStatic private external fun nativeVersion(): Int
        @JvmStatic private external fun nativeInit(osr: Int, flags: Int): Int
        @JvmStatic private external fun nativeAlloc(osr: Int, flags: Int): Long
        @JvmStatic private external fun nativeFree(handle: Long)
        @JvmStatic private external fun nativeRead(handle: Long, out: ByteArray): Int
        @JvmStatic private external fun nativeStatus(handle: Long): String?
    }
}
