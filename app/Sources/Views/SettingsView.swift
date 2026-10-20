import SwiftUI

/// The settings that matter on an iPad, read from and written to RPCS3's config.yml through
/// the core. Paths are RPCS3's own section/key names.
struct SettingsView: View {
    @EnvironmentObject var emulator: Emulator
    @Environment(\.dismiss) private var dismiss
    @AppStorage("touchOpacity") private var touchOpacity = 0.45
    @State private var confirmReset = false

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    ConfigPicker(title: "Resolution scale", path: "Video/Resolution Scale", options: ["50", "75", "100", "150", "200"], suffix: "%")
                    ConfigPicker(title: "Frame limit", path: "Video/Frame limit", options: ["Auto", "30", "60", "Off"])
                    ConfigPicker(title: "Shaders", path: "Video/Shader Mode", options: [
                        "Async Recompiler (multi-threaded)",
                        "Async Recompiler with Shader Interpreter",
                        "Legacy Recompiler (single-threaded)",
                    ])
                    ConfigPicker(title: "Shader compiler threads", path: "Video/Shader Compiler Threads", options: ["1", "2", "0"])
                    ConfigToggle(title: "Performance overlay", path: "Video/Performance Overlay/Enabled")
                    ConfigToggle(title: "Write color buffers", path: "Video/Write Color Buffers")
                    ConfigToggle(title: "Strict rendering mode", path: "Video/Strict Rendering Mode")
                } header: {
                    Text("Graphics")
                } footer: {
                    Text("Every 25% above 100% costs roughly 50% more GPU memory. On a 4 GB iPad stay at 100% or below.")
                }

                Section {
                    ConfigPicker(title: "SPU block size", path: "Core/SPU Block Size", options: ["Safe", "Mega", "Giga"])
                    ConfigPicker(title: "LLVM compile threads", path: "Core/Max LLVM Compile Threads", options: ["1", "2", "0"])
                    ConfigToggle(title: "Precompile all PPU modules", path: "Core/LLVM Precompilation")
                    ConfigToggle(title: "SPU cache", path: "Core/SPU Cache")
                } header: {
                    Text("CPU")
                } footer: {
                    Text("Each LLVM compile thread can use several hundred MB while a game's code is first compiled. 1 thread is slower to start but keeps a 4 GB iPad from being killed for memory.")
                }

                Section("Audio") {
                    ConfigPicker(title: "Output", path: "Audio/Renderer", options: ["Cubeb", "Null"])
                    ConfigPicker(title: "Buffer (ms)", path: "Audio/Desired Audio Buffer Duration", options: ["32", "64", "100", "150"])
                }

                Section("Controls") {
                    VStack(alignment: .leading) {
                        Text("Touch controls opacity")
                        Slider(value: $touchOpacity, in: 0.1...1)
                    }
                }

                Section {
                    Button("Restore 4 GB iPad defaults", role: .destructive) { confirmReset = true }
                } footer: {
                    Text("Resets CPU, GPU and audio settings to the low-memory profile Ips3 applies on first launch.")
                }

                if let preflight = emulator.preflight {
                    Section("Device") {
                        LabeledContent("RAM", value: "\(preflight.physicalMemory >> 20) MB")
                        LabeledContent("App memory limit", value: "\(preflight.memoryLimit >> 20) MB")
                        LabeledContent("Address space", value: "\(preflight.addressSpaceGB) GB")
                        LabeledContent("JIT", value: preflight.jitEnabled ? "Enabled" : "Disabled")
                        LabeledContent("Firmware", value: emulator.firmwareVersion.isEmpty ? "Not installed" : emulator.firmwareVersion)
                    }
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .confirmationDialog("Restore defaults?", isPresented: $confirmReset) {
                Button("Restore", role: .destructive) { emulator.resetMemoryProfile() }
            }
        }
    }
}

private struct ConfigPicker: View {
    @EnvironmentObject var emulator: Emulator
    let title: String
    let path: String
    let options: [String]
    var suffix = ""

    var body: some View {
        let current = emulator.config(path)
        let all = options.contains(current) || current.isEmpty ? options : [current] + options

        Picker(title, selection: Binding(get: { current }, set: { emulator.setConfig(path, $0) })) {
            ForEach(all, id: \.self) { option in
                Text(label(option)).tag(option)
            }
        }
    }

    private func label(_ option: String) -> String {
        if path.hasSuffix("Threads") && option == "0" { return "Auto" }
        return option + suffix
    }
}

private struct ConfigToggle: View {
    @EnvironmentObject var emulator: Emulator
    let title: String
    let path: String

    var body: some View {
        Toggle(title, isOn: Binding(
            get: { emulator.config(path) == "true" },
            set: { emulator.setConfig(path, $0 ? "true" : "false") }
        ))
    }
}
