import QuartzCore
import SwiftUI
import UIKit

/// A UIView backed by the CAMetalLayer that MoltenVK presents RPCS3's frames into.
final class MetalLayerView: UIView {
    override class var layerClass: AnyClass { CAMetalLayer.self }

    var metalLayer: CAMetalLayer { layer as! CAMetalLayer }
    weak var core: CoreLibrary?

    override init(frame: CGRect) {
        super.init(frame: frame)
        backgroundColor = .black
        isMultipleTouchEnabled = false
        metalLayer.pixelFormat = .bgra8Unorm
        metalLayer.framebufferOnly = true
        metalLayer.isOpaque = true
        // ProMotion iPads would otherwise present at 120 Hz and halve the frame budget.
        metalLayer.maximumDrawableCount = 3
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    private var pixelSize: (Int32, Int32) {
        let scale = window?.screen.nativeScale ?? UIScreen.main.nativeScale
        return (Int32((bounds.width * scale).rounded()), Int32((bounds.height * scale).rounded()))
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        guard let core else { return }

        if window != nil {
            contentScaleFactor = window?.screen.nativeScale ?? 2
            let (w, h) = pixelSize
            metalLayer.drawableSize = CGSize(width: Int(w), height: Int(h))
            core.setSurface(Unmanaged.passUnretained(metalLayer).toOpaque(), w, h)
        } else {
            core.setSurface(nil, 0, 0)
        }
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        guard let core, window != nil else { return }
        let (w, h) = pixelSize
        guard w > 0, h > 0 else { return }
        core.surfaceResized(w, h)
    }
}

struct MetalView: UIViewRepresentable {
    let core: CoreLibrary

    func makeUIView(context: Context) -> MetalLayerView {
        let view = MetalLayerView(frame: .zero)
        view.core = core
        return view
    }

    func updateUIView(_ uiView: MetalLayerView, context: Context) {}

    static func dismantleUIView(_ uiView: MetalLayerView, coordinator: ()) {
        uiView.core?.setSurface(nil, 0, 0)
    }
}
