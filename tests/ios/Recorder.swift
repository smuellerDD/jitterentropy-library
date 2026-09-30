/*
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import Foundation

/// Records the raw noise data of the SP800-90B and NTG.1 assessments in the
/// app, as the recording scripts of tests/raw-entropy/recording_userspace do
/// on a host: the same sets, the same file names, one directory the
/// validation scripts read as it is. See tests/raw-entropy/recording_library/README.md.
///
/// The recording goes to `dir`.partial beside `dir` and replaces `dir` only
/// once it is complete, so that a cancelled or failed one is never taken for
/// data.
///
/// Calls into the library: run it on the queue every other call runs on.
struct Recorder {
    /// `repeats` files named `name`-0001.data onwards, of `rounds` time deltas
    /// each, as jitterentropy-hashtime <rounds> <repeats> <name> writes them.
    /// `source` is a JENT_RECORD_* value of jitterentropy-record.h.
    struct DataSet {
        let name: String
        let source: Int32
        let rounds: UInt32
        let repeats: Int
    }

    /// A recording that failed or was cancelled; nothing of it is kept.
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    /// The directory the validation scripts read: tests/raw-entropy/results-measurements.
    static let dir = "results-measurements"

    /* NUM_EVENTS, NUM_EVENTS_RESTART and NUM_RESTART of invoke_testing_helper.sh */
    private static let events: UInt32 = 1_000_000
    private static let eventsRestart: UInt32 = 1000
    private static let restarts = 1000

    /// What invoke_testing.sh (default), invoke_testing_fips.sh and
    /// invoke_testing_ntg1.sh record, in their order.
    static func sets(_ mode: Collector.Mode) -> [DataSet] {
        let common = [
            DataSet(name: "jent-raw-noise", source: JENT_RECORD_COMMON,
                    rounds: events, repeats: 1),
            DataSet(name: "jent-raw-noise-restart", source: JENT_RECORD_COMMON,
                    rounds: eventsRestart, repeats: restarts),
        ]
        guard mode == .ntg1 else { return common }

        return common + [
            DataSet(name: "jent-raw-noise_hashloop", source: JENT_RECORD_HASHLOOP,
                    rounds: events, repeats: 1),
            DataSet(name: "jent-raw-noise_memaccloop", source: JENT_RECORD_MEMACCESS,
                    rounds: events, repeats: 1),
            DataSet(name: "jent-raw-noise-hashloop-restart", source: JENT_RECORD_HASHLOOP,
                    rounds: eventsRestart, repeats: restarts),
            DataSet(name: "jent-raw-noise-memaccloop-restart", source: JENT_RECORD_MEMACCESS,
                    rounds: eventsRestart, repeats: restarts),
        ]
    }

    let base: URL

    /// Records the sets of `config`'s mode with its flags and oversampling
    /// rate. `cancelled` is asked between files, `progress` told the share of
    /// the samples recorded so far and the file at hand. `description` heads
    /// the recording.txt left beside the data. Returns the directory, and the
    /// health test failures per set.
    func record(_ config: Collector.Config, description: String,
                cancelled: () -> Bool,
                progress: (Double, String) -> Void) throws -> (URL, [(String, String)]) {
        let fm = FileManager.default
        let sets = Self.sets(config.mode)
        let total = sets.reduce(0.0) { $0 + Double($1.rounds) * Double($1.repeats) }
        let partial = base.appendingPathComponent(Self.dir + ".partial")
        var health: [(String, String)] = []
        var done = 0.0

        try? fm.removeItem(at: partial)
        try fm.createDirectory(at: partial, withIntermediateDirectories: true)

        do {
            for dataSet in sets {
                var failures: UInt32 = 0
                for i in 1...dataSet.repeats {
                    if cancelled() {
                        throw Failure(description: "recording cancelled")
                    }

                    let name = String(format: "%@-%04ld.data", dataSet.name, i)
                    let file = partial.appendingPathComponent(name)
                    progress(done / total, name)

                    var result = jent_record_result()
                    let ret = jent_record_raw(file.path, dataSet.rounds, config.osr,
                                              config.flags, UInt32(dataSet.source), 0,
                                              &result, nil, 0)
                    let err = errno
                    if ret != JENT_RECORD_OK {
                        throw Failure(description:
                            "recording \(name) failed (\(config)): \(Self.describe(ret, err))")
                    }
                    failures |= result.health_failure
                    done += Double(dataSet.rounds)
                }
                health.append((dataSet.name, failures == 0 ? "none" : Self.healthNames(failures)))
            }

            var text = description + "\n"
            text += "Configuration: \(config)\n"
            text += String(format: "Flags: 0x%08x\n", config.flags)
            for (dataSet, (_, failures)) in zip(sets, health) {
                text += "\(dataSet.name): \(dataSet.repeats) x \(dataSet.rounds) time deltas, " +
                        "health test failures: \(failures)\n"
            }
            try text.write(to: partial.appendingPathComponent("recording.txt"),
                           atomically: true, encoding: .utf8)
        } catch {
            try? fm.removeItem(at: partial)
            throw error
        }

        let directory = base.appendingPathComponent(Self.dir)
        try? fm.removeItem(at: directory)
        try fm.moveItem(at: partial, to: directory)
        progress(1, "")

        return (directory, health)
    }

    /// The names of the health tests in a jent_health_failure() value.
    private static func healthNames(_ failures: UInt32) -> String {
        var buf = [CChar](repeating: 0, count: 128)
        _ = jent_record_health_names(failures, &buf, buf.count)
        return String(cString: buf)
    }

    /// Names a JENT_RECORD_* code of jent_record_raw(), with the errno of EIO.
    private static func describe(_ code: Int32, _ err: Int32) -> String {
        switch code {
        case JENT_RECORD_EINVAL: return "invalid noise source or sample count"
        case JENT_RECORD_ENOMEM: return "no memory for the samples"
        case JENT_RECORD_ECOLLECTOR:
            return "no collector: the self tests failed, the flags were refused, " +
                   "or the memory could not be allocated or locked"
        case JENT_RECORD_EIO:
            return "the file could not be written: \(String(cString: strerror(err))) (\(err))"
        default: return "unknown error (\(code))"
        }
    }
}
