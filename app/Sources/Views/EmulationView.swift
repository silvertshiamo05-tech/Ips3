import SwiftUI

struct EmulationView: View {
    @EnvironmentObject var emulator: Emulator
    @ObservedObject var input: InputManager
    let game: Game

    @State private var showMenu = false
    @AppStorage("showFPS") private var showFPS = true
    @AppStorage("touchOpacity") private var touchOpacity = 0.45

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            if let core = emulator.core {
                MetalView(core: core)
                    .aspectRatio(16 / 9, contentMode: .fit)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .ignoresSafeArea()
            }

            if input.showTouchControls {
                TouchControlsView(input: input, opacity: touchOpacity)
            }

            VStack {
                HStack {
                    if showFPS {
                        Text(String(format: "%.0f FPS", emulator.fps))
                            .font(.caption.monospacedDigit().bold())
                            .padding(.horizontal, 8)
                            .padding(.vertical, 4)
                            .background(.black.opacity(0.5), in: Capsule())
                            .foregroundStyle(.white)
                    }
                    Spacer()
                    Button {
                        emulator.pause()
                        showMenu = true
                    } label: {
                        Image(systemName: "line.3.horizontal.circle.fill")
                            .font(.title)
                            .foregroundStyle(.white.opacity(0.8))
                    }
                }
                .padding()
                Spacer()
            }
        }
        .statusBarHidden()
        .persistentSystemOverlays(.hidden)
        .confirmationDialog(game.title, isPresented: $showMenu, titleVisibility: .visible) {
            Button("Resume") { emulator.resume() }
            Button("PS Home Menu") {
                emulator.resume()
                emulator.homeMenu()
            }
            Button(input.showTouchControls ? "Hide Touch Controls" : "Show Touch Controls") {
                input.showTouchControls.toggle()
                emulator.resume()
            }
            Button(showFPS ? "Hide FPS" : "Show FPS") {
                showFPS.toggle()
                emulator.resume()
            }
            Button("Stop Game", role: .destructive) { emulator.stop() }
            Button("Cancel", role: .cancel) { emulator.resume() }
        }
    }
}
