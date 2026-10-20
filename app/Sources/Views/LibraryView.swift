import SwiftUI
import UniformTypeIdentifiers

struct LibraryView: View {
    @EnvironmentObject var emulator: Emulator
    @State private var importing = false
    @State private var importFolders = false
    @State private var showSettings = false
    @State private var showLog = false

    private let columns = [GridItem(.adaptive(minimum: 150, maximum: 220), spacing: 20)]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 20) {
                    if emulator.firmwareVersion.isEmpty {
                        FirmwareBanner { importFolders = false; importing = true }
                    }

                    if emulator.games.isEmpty {
                        EmptyLibrary()
                    } else {
                        LazyVGrid(columns: columns, spacing: 24) {
                            ForEach(emulator.games) { game in
                                GameTile(game: game)
                                    .onTapGesture { emulator.boot(game) }
                            }
                        }
                    }
                }
                .padding()
            }
            .navigationTitle("Ips3")
            .refreshable { emulator.refresh() }
            .toolbar {
                ToolbarItemGroup(placement: .topBarLeading) {
                    if !emulator.firmwareVersion.isEmpty {
                        Text("FW \(emulator.firmwareVersion)")
                            .font(.footnote)
                            .foregroundStyle(.secondary)
                    }
                }
                ToolbarItemGroup(placement: .topBarTrailing) {
                    Menu {
                        Button("Import File (PUP, PKG, RAP, ISO)…", systemImage: "doc.badge.plus") {
                            importFolders = false
                            importing = true
                        }
                        Button("Import Game Folder…", systemImage: "folder.badge.plus") {
                            importFolders = true
                            importing = true
                        }
                    } label: {
                        Image(systemName: "plus")
                    }
                    Button { showLog = true } label: { Image(systemName: "doc.text.magnifyingglass") }
                    Button { showSettings = true } label: { Image(systemName: "gearshape") }
                }
            }
            // One importer for both modes: SwiftUI only honours a single .fileImporter per view.
            .fileImporter(isPresented: $importing, allowedContentTypes: importFolders ? [.folder] : [.data, .item], allowsMultipleSelection: true) { result in
                if case .success(let urls) = result {
                    for url in urls {
                        emulator.importItem(at: url)
                    }
                }
            }
            .sheet(isPresented: $showSettings) { SettingsView() }
            .sheet(isPresented: $showLog) { LogView() }
            .overlay {
                if let message = emulator.busyMessage {
                    BusyOverlay(message: message, progress: emulator.progress)
                }
            }
            .alert("Ips3", isPresented: Binding(get: { emulator.lastError != nil }, set: { if !$0 { emulator.lastError = nil } })) {
                Button("OK", role: .cancel) {}
            } message: {
                Text(emulator.lastError ?? "")
            }
        }
    }
}

private struct FirmwareBanner: View {
    let action: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("PS3 firmware required", systemImage: "exclamationmark.triangle.fill")
                .font(.headline)
                .foregroundStyle(.orange)
            Text("Download PS3UPDAT.PUP from Sony's PlayStation 3 system software page and import it here. Firmware is never bundled with Ips3.")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Button("Import PS3UPDAT.PUP…", action: action)
                .buttonStyle(.borderedProminent)
        }
        .padding()
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.orange.opacity(0.1), in: RoundedRectangle(cornerRadius: 14))
    }
}

private struct EmptyLibrary: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "opticaldisc")
                .font(.system(size: 56))
                .foregroundStyle(.secondary)
            Text("No games yet")
                .font(.title3.bold())
            Text("Import a .pkg, an .iso, or a game folder (with PS3_GAME/ inside). You can also copy them into Documents/RPCS3/games with the Files app.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 480)
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 80)
    }
}

private struct GameTile: View {
    let game: Game

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Group {
                if let image = UIImage(contentsOfFile: game.icon) {
                    Image(uiImage: image)
                        .resizable()
                        .aspectRatio(320 / 176, contentMode: .fit)
                } else {
                    RoundedRectangle(cornerRadius: 8)
                        .fill(Color.gray.opacity(0.2))
                        .aspectRatio(320 / 176, contentMode: .fit)
                        .overlay(Image(systemName: "gamecontroller").font(.largeTitle).foregroundStyle(.secondary))
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8))

            Text(game.title)
                .font(.subheadline.bold())
                .lineLimit(2)
            Text([game.titleID, game.kind.uppercased()].filter { !$0.isEmpty }.joined(separator: " · "))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .contentShape(Rectangle())
    }
}

private struct BusyOverlay: View {
    let message: String
    let progress: Double?

    var body: some View {
        ZStack {
            Color.black.opacity(0.4).ignoresSafeArea()
            VStack(spacing: 14) {
                if let progress {
                    ProgressView(value: progress)
                        .frame(width: 240)
                } else {
                    ProgressView()
                }
                Text(message)
                    .font(.callout)
                    .multilineTextAlignment(.center)
            }
            .padding(24)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 16))
        }
    }
}
