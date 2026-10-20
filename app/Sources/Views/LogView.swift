import SwiftUI

struct LogView: View {
    @EnvironmentObject var emulator: Emulator
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            ScrollViewReader { proxy in
                List(emulator.logs) { line in
                    Text(line.text)
                        .font(.caption.monospaced())
                        .foregroundStyle(line.level <= 1 ? .red : line.level == 2 ? .orange : .primary)
                        .textSelection(.enabled)
                        .id(line.id)
                }
                .listStyle(.plain)
                .onAppear {
                    if let last = emulator.logs.last { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
            .overlay {
                if emulator.logs.isEmpty {
                    ContentUnavailableView("No warnings or errors", systemImage: "checkmark.seal", description: Text("The full log is written to Library/Caches/RPCS3/Ips3.log."))
                }
            }
            .navigationTitle("Log")
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    if let url = emulator.logFileURL {
                        ShareLink(item: url) { Label("Share full log", systemImage: "square.and.arrow.up") }
                    }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }
}
