import SwiftUI

/// Shown when the preflight checks fail or the core cannot be loaded: says exactly which
/// requirement is missing instead of letting RPCS3 crash on its first allocation.
struct SetupView: View {
    @EnvironmentObject var emulator: Emulator
    var failure: String?

    var body: some View {
        NavigationStack {
            List {
                if let failure {
                    Section("Error") {
                        Text(failure)
                            .font(.callout)
                            .foregroundStyle(.red)
                            .textSelection(.enabled)
                    }
                }

                if let preflight = emulator.preflight {
                    Section("Requirements") {
                        ForEach(preflight.checks) { check in
                            HStack(alignment: .top, spacing: 12) {
                                Image(systemName: check.passed ? "checkmark.circle.fill" : (check.required ? "xmark.octagon.fill" : "exclamationmark.triangle.fill"))
                                    .foregroundStyle(check.passed ? .green : (check.required ? .red : .orange))
                                    .font(.title3)
                                VStack(alignment: .leading, spacing: 4) {
                                    Text(check.title).font(.headline)
                                    Text(check.detail).font(.footnote).foregroundStyle(.secondary)
                                }
                            }
                            .padding(.vertical, 4)
                        }
                    }
                }

                Section("How to run Ips3") {
                    Text("1. Import Ips3.ipa into LiveContainer.")
                    Text("2. Make sure LiveContainer itself is signed with the Extended Virtual Addressing and Increased Memory Limit entitlements (SideStore/AltStore can add both).")
                    Text("3. Start Ips3 with \"Launch with JIT\" (StikDebug or SideStore's JIT).")
                    Text("4. Import PS3UPDAT.PUP, then your games.")
                }
                .font(.callout)

                Section {
                    Button("Check Again") { emulator.start() }
                    Button("Load Anyway", role: .destructive) { emulator.start(force: true) }
                } footer: {
                    Text("Loading anyway will most likely crash if a required check failed.")
                }
            }
            .navigationTitle("Ips3 Setup")
        }
    }
}
