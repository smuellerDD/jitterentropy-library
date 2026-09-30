/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import Combine
import Foundation
import UIKit

/// One Jitter RNG entropy collector, replaceable by one in another mode, on
/// the timer thread or with other initial settings.
///
/// Every call into the library runs on the serial `queue`: it keeps slow
/// calls off the UI, and a collector belongs to one thread at a time.
final class Collector: ObservableObject {
    /// The mode flags. Both compliance modes imply JENT_FORCE_SECURE_MEM, so
    /// they fail with EMEM where the state cannot be locked.
    enum Mode: CustomStringConvertible {
        case standard, fips, ntg1

        var flags: UInt32 {
            switch self {
            case .standard: return 0
            case .fips: return UInt32(JENT_FORCE_FIPS)
            case .ntg1: return UInt32(JENT_NTG1)
            }
        }

        var description: String {
            switch self {
            case .standard: return "default"
            case .fips: return "FIPS"
            case .ntg1: return "NTG.1"
            }
        }
    }

    /// A mode, whether time stamps come from the library's timer thread
    /// rather than the platform clock (JENT_FORCE_INTERNAL_TIMER), and the
    /// initial oversampling rate, memory size and hash loop count.
    ///
    /// A health test failure raises the oversampling rate and the hash loop
    /// count of the replacement collector. It does not raise the memory
    /// size once one has been chosen here: jent_update_memsize() applies its
    /// increment only to a size the library derived itself, so leaving
    /// `memSize` at 0 (automatic) is what lets the recovery grow it.
    ///
    /// `memSize` is the JENT_MAX_MEMSIZE field: 0 derives the size from the
    /// CPU caches, n is 2^(n + 9) bytes. `hashLoop` is the JENT_HASHLOOP
    /// field: 0 is the library's default count, n is 2^(n - 1) loops.
    ///
    /// The choice is per collector. NTG.1 forbids the timer thread.
    struct Config: CustomStringConvertible {
        let mode: Mode
        let timerThread: Bool
        let osr: UInt32
        let memSize: UInt32
        let hashLoop: UInt32

        var flags: UInt32 {
            mode.flags | (timerThread ? UInt32(JENT_FORCE_INTERNAL_TIMER) : 0) |
                memSize << UInt32(JENT_FLAGS_TO_MEMSIZE_SHIFT) |
                hashLoop << UInt32(JENT_FLAGS_TO_HASHLOOP_SHIFT)
        }

        var description: String {
            (timerThread ? "\(mode), timer thread" : "\(mode)") +
                ", osr \(osr), memory \(Collector.memSizeLabel(memSize)), " +
                "hash loop \(Collector.hashLoopLabel(hashLoop))"
        }
    }

    @Published private(set) var state = ""
    @Published private(set) var output = ""
    @Published private(set) var ready = false
    @Published private(set) var allocating = false

    /// Whether the next collector uses the timer thread.
    @Published var timerThread = false
    /// The initial oversampling rate, memory size and hash loop count of the
    /// next collector.
    @Published var osr = Collector.minOsr
    @Published var memSize: UInt32 = 0
    @Published var hashLoop: UInt32 = 0

    /// A raw noise recording running, its share done, and what it reported last.
    @Published private(set) var recording = false
    @Published private(set) var recordProgress = 0.0
    @Published private(set) var recordState = ""

    /* Set on the main queue, read on `queue` between the files of a recording. */
    private let cancelLock = NSLock()
    private var cancelRequested = false

    /* JENT_MIN_OSR and JENT_MAX_OSR, which jitterentropy.h does not export. */
    static let minOsr: UInt32 = 3
    static let maxOsr: UInt32 = 20
    /*
     * The JENT_MAX_MEMSIZE_512MB and JENT_HASHLOOP_128 field values.
     */
    static let maxMemSize: UInt32 = 20
    static let maxHashLoop: UInt32 = 8

    /// A JENT_MAX_MEMSIZE field value as a size.
    static func memSizeLabel(_ memSize: UInt32) -> String {
        if memSize == 0 { return "automatic" }
        return memSize < 11 ? "\(1 << (memSize - 1)) kB" : "\(1 << (memSize - 11)) MB"
    }

    /// A JENT_HASHLOOP field value as a loop count.
    static func hashLoopLabel(_ hashLoop: UInt32) -> String {
        hashLoop == 0 ? "default" : "\(1 << (hashLoop - 1))"
    }

    private let queue = DispatchQueue(label: "de.chronox.jitterentropy.example")
    /* struct rand_data *, replaced in place by jent_read_entropy_safe(). */
    private var ec: OpaquePointer?

    static var version: String {
        let v = jent_version()
        return "\(v / 1_000_000).\(v / 10_000 % 100).\(v / 100 % 100)"
    }

    init() {
        allocate(.standard)
    }

    deinit {
        let ec = self.ec
        queue.async { jent_entropy_collector_free(ec) }
    }

    /// Replaces the collector with one in `mode` and the settings above.
    /// Called on the main queue.
    func allocate(_ mode: Mode) {
        let config = Config(mode: mode, timerThread: timerThread, osr: osr,
                            memSize: memSize, hashLoop: hashLoop)

        state = "Allocating the entropy collector (\(config))…"
        output = ""
        ready = false
        allocating = true
        queue.async { self.replace(config) }
    }

    private func replace(_ config: Config) {
        jent_entropy_collector_free(ec)
        ec = nil

        /* The collector must be allocated with what the power-on tests ran with. */
        let text: String
        let ret = jent_entropy_init_ex(config.osr, config.flags)

        if ret != 0 {
            text = "jent_entropy_init_ex failed (\(config)): \(Self.describe(ret)) (\(ret))"
        } else {
            ec = jent_entropy_collector_alloc(config.osr, config.flags)
            text = ec != nil ? "Entropy collector allocated (\(config))"
                             : "jent_entropy_collector_alloc failed (\(config))"
        }

        NSLog("%@", text)
        let ready = ec != nil
        DispatchQueue.main.async {
            self.state = text
            self.ready = ready
            self.allocating = false
        }
    }

    func showStatus() {
        queue.async {
            /* The size the library documents as holding the whole document. */
            var buf = [CChar](repeating: 0, count: 4096)
            let text = jent_status(self.ec, &buf, buf.count) == 0
                ? String(cString: buf) : "jent_status failed"

            NSLog("status: %@", text)
            self.show(text)
        }
    }

    func generate() {
        queue.async {
            var buf = [CChar](repeating: 0, count: 32)
            let ret = jent_read_entropy_safe(&self.ec, &buf, buf.count)
            let text: String

            if ret < 0 {
                text = "jent_read_entropy_safe failed: \(Self.describe(Int32(ret))) (\(ret))"
            } else {
                text = buf.map { String(format: "%02x", UInt8(bitPattern: $0)) }
                          .joined()
            }

            /* Do not leave the output behind in the buffer. */
            _ = buf.withUnsafeMutableBytes { memset_s($0.baseAddress, $0.count, 0, $0.count) }

            NSLog("32 bytes: %@", text)
            self.show(text)
        }
    }

    /// Records the raw noise data of `mode` with the settings above into
    /// Documents/results-measurements; see `Recorder`. Called on the main
    /// queue.
    func record(_ mode: Mode) {
        let config = Config(mode: mode, timerThread: timerThread, osr: osr,
                            memSize: memSize, hashLoop: hashLoop)
        let documents = FileManager.default.urls(for: .documentDirectory,
                                                 in: .userDomainMask)[0]
        let description = Self.recordingDescription()

        setCancel(false)
        recording = true
        recordProgress = 0
        recordState = "Recording (\(config))…"
        /*
         * A phone that locks suspends the app, and what runs while it is
         * suspended is not the app's timing.
         */
        UIApplication.shared.isIdleTimerDisabled = true

        queue.async {
            let text: String
            do {
                let (directory, health) = try Recorder(base: documents).record(
                    config, description: description,
                    cancelled: { self.isCancelled() }) { share, file in
                        DispatchQueue.main.async {
                            self.recordProgress = share
                            if !file.isEmpty {
                                self.recordState = "Recording (\(config)): \(file)"
                            }
                        }
                    }
                text = "Recorded (\(config)) into \(directory.path)\n" +
                    "Health test failures:\n" +
                    health.map { "\($0.0): \($0.1)" }.joined(separator: "\n")
            } catch {
                text = "\(error)"
            }

            NSLog("%@", text)
            DispatchQueue.main.async {
                self.recordState = text
                self.recording = false
                UIApplication.shared.isIdleTimerDisabled = false
            }
        }
    }

    /// Stops the recording before its next file; it records nothing then.
    func cancelRecord() {
        setCancel(true)
    }

    private func setCancel(_ value: Bool) {
        cancelLock.lock()
        cancelRequested = value
        cancelLock.unlock()
    }

    private func isCancelled() -> Bool {
        cancelLock.lock()
        defer { cancelLock.unlock() }
        return cancelRequested
    }

    /// What recorded the data, for the recording.txt beside it.
    private static func recordingDescription() -> String {
        var info = utsname()
        uname(&info)
        /* The model identifier, "iPhone16,1", which names the chip as well. */
        let machine = withUnsafeBytes(of: &info.machine) {
            String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self)
        }
        let device = UIDevice.current

        return "Recorded by the Jitter RNG \(version) example app on \(Date())\n" +
            "Device: \(device.model) \(machine)\n" +
            "\(device.systemName) \(device.systemVersion)"
    }

    private func show(_ text: String) {
        DispatchQueue.main.async { self.output = text }
    }

    /// Names a return code of the library.
    private static func describe(_ code: Int32) -> String {
        switch code {
        /* jent_entropy_init_ex() */
        case 1: return "ENOTIME: timer service not available"
        case 2: return "ECOARSETIME: timer too coarse"
        case 3: return "ENOMONOTONIC: timer not monotonic"
        case 6: return "EMINVARVAR: timer variations too small"
        case 8: return "ESTUCK: too many stuck results"
        case 9: return "EHEALTH: health test failed"
        case 10: return "ERCT: RCT failed"
        case 11: return "EHASH: hash self test failed"
        case 12: return "EMEM: cannot allocate memory"
        case 13: return "EGCD: GCD self test failed"
        /* jent_read_entropy() */
        case -1: return "JENT_ERR_EINVAL: invalid collector"
        case -2: return "JENT_ERR_RCT: intermittent RCT failure"
        case -3: return "JENT_ERR_APT: intermittent APT failure"
        case -4: return "JENT_ERR_NOTIME: timer cannot be initialized"
        case -5: return "JENT_ERR_LAG: intermittent lag predictor failure"
        case -6: return "JENT_ERR_RCT_PERMANENT: permanent RCT failure"
        case -7: return "JENT_ERR_APT_PERMANENT: permanent APT failure"
        case -8: return "JENT_ERR_LAG_PERMANENT: permanent lag predictor failure"
        case -9: return "JENT_ERR_RCT_MEM: intermittent RCT with memory failure"
        case -10: return "JENT_ERR_RCT_MEM_PERMANENT: permanent RCT with memory failure"
        case -11: return "JENT_ERR_SELFTEST: self test failed"
        default: return "unknown error"
        }
    }
}
