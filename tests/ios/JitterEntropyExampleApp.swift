/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 */

import SwiftUI

/// Allocates one collector at start-up. The collector tab replaces it with a
/// new one: without flags, in FIPS or in NTG.1 mode, on the platform clock or
/// on the library's timer thread, and with the oversampling rate, memory size
/// and hash loop count its controls say. The output tab shows its status and
/// 32 bytes of its output, each on a button press. The record tab records the
/// raw noise data of a mode, with the same settings, for the entropy
/// assessment.
@main
struct JitterEntropyExampleApp: App {
    @StateObject private var collector = Collector()

    var body: some Scene {
        WindowGroup {
            ContentView(collector: collector)
        }
    }
}

struct ContentView: View {
    @ObservedObject var collector: Collector

    var body: some View {
        TabView {
            CollectorTab(collector: collector)
                .tabItem { Label("Collector", systemImage: "slider.horizontal.3") }
            OutputTab(collector: collector)
                .tabItem { Label("Output", systemImage: "text.alignleft") }
            RecordTab(collector: collector)
                .tabItem { Label("Record", systemImage: "waveform") }
        }
    }
}

/// The version and the state of the current collector, atop both tabs.
struct Header: View {
    @ObservedObject var collector: Collector

    var body: some View {
        Text("Jitter RNG \(Collector.version)")
            .font(.title2)
        Text(collector.state)
    }
}

/// The settings of the next collector and the buttons that allocate it.
struct CollectorTab: View {
    @ObservedObject var collector: Collector

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Header(collector: collector)

                Text("New collector")
                    .font(.subheadline)
                    .foregroundStyle(.secondary)
                Toggle("Timer thread", isOn: $collector.timerThread)
                    .disabled(collector.allocating)
                Group {
                    Stepper("Initial oversampling rate: \(collector.osr)",
                            value: $collector.osr,
                            in: Collector.minOsr...Collector.maxOsr)
                    Stepper("Memory size: \(Collector.memSizeLabel(collector.memSize))",
                            value: $collector.memSize,
                            in: 0...Collector.maxMemSize)
                    Stepper("Hash loop count: \(Collector.hashLoopLabel(collector.hashLoop))",
                            value: $collector.hashLoop,
                            in: 0...Collector.maxHashLoop)
                }
                .disabled(collector.allocating)
                HStack(spacing: 12) {
                    Button("Default") { collector.allocate(.standard) }
                        .frame(maxWidth: .infinity)
                    Button("FIPS") { collector.allocate(.fips) }
                        .frame(maxWidth: .infinity)
                    /* NTG.1 forbids the timer thread; the library refuses it. */
                    Button("NTG.1") { collector.allocate(.ntg1) }
                        .frame(maxWidth: .infinity)
                        .disabled(collector.timerThread)
                }
                .buttonStyle(.bordered)
                /* Behind a recording on the queue, it would wait minutes. */
                .disabled(collector.allocating || collector.recording)
                if collector.timerThread {
                    Text("Collectors allocated while this is on use the timer thread, the others the platform clock. NTG.1 forbids it.")
                        .font(.footnote)
                        .foregroundStyle(.secondary)
                }
            }
            .padding()
        }
    }
}

/// The buttons that use the collector, and what they returned.
struct OutputTab: View {
    @ObservedObject var collector: Collector

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Header(collector: collector)

            HStack(spacing: 12) {
                Button("Show status") { collector.showStatus() }
                    .frame(maxWidth: .infinity)
                Button("32 bytes") { collector.generate() }
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(!collector.ready || collector.recording)

            ScrollView {
                Text(collector.output)
                    .font(.system(.footnote, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
    }
}

/// The buttons that record the raw noise data of a mode, and how far the
/// recording is.
struct RecordTab: View {
    @ObservedObject var collector: Collector

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Header(collector: collector)

            Text("Records the raw noise data of the entropy assessment in the mode chosen below, with the timer, oversampling rate, memory size and hash loop count of the Collector tab, into Documents/\(Recorder.dir). That takes minutes; keep the app in the foreground.")
                .font(.footnote)
                .foregroundStyle(.secondary)

            HStack(spacing: 12) {
                Button("Default") { collector.record(.standard) }
                    .frame(maxWidth: .infinity)
                Button("FIPS") { collector.record(.fips) }
                    .frame(maxWidth: .infinity)
                /* NTG.1 forbids the timer thread; the library refuses it. */
                Button("NTG.1") { collector.record(.ntg1) }
                    .frame(maxWidth: .infinity)
                    .disabled(collector.timerThread)
            }
            .buttonStyle(.bordered)
            .disabled(collector.recording || collector.allocating)

            if collector.recording {
                ProgressView(value: collector.recordProgress)
                Button("Cancel") { collector.cancelRecord() }
                    .frame(maxWidth: .infinity)
                    .buttonStyle(.bordered)
            }

            ScrollView {
                Text(collector.recordState)
                    .font(.system(.footnote, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding()
    }
}
