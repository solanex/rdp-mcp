import AppKit
import Carbon.HIToolbox
import CRdpMcpCore

/// Toont het remote scherm en geeft muis en toetsenbord door.
/// Toetsen komen binnen via SessionManager (een lokale event-monitor), zodat ook
/// Cmd-combinaties en hun keyUp aankomen.
final class RDPView: NSView {
    weak var session: RDPSession?
    var cmdAsWin = false
    /// Deze tab is zichtbaar
    var isActive = false {
        didSet {
            isHidden = !isActive
            if isActive { focusIfActive() } else { releaseKeys() }
        }
    }

    private var bitmap: CGContext?
    private var remoteW = 0
    private var remoteH = 0
    private var scrollAccumX: CGFloat = 0
    private var scrollAccumY: CGFloat = 0
    private var resizeWork: DispatchWorkItem?
    private let overlay = AIOverlayView()
    private var resignObserver: NSObjectProtocol?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layerContentsRedrawPolicy = .never
        layer?.backgroundColor = NSColor.black.cgColor
        layer?.contentsGravity = .resizeAspect
        layer?.magnificationFilter = .linear
        layer?.minificationFilter = .trilinear
        overlay.frame = bounds
        overlay.autoresizingMask = [.width, .height]
        overlay.isHidden = true
        addSubview(overlay)
    }

    @available(*, unavailable)
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    // MARK: - beeld

    func pullFrame() {
        guard let h = session?.handle else { return }
        var w: Int32 = 0
        var ht: Int32 = 0
        var dirty = rdpmcp_rect()
        var resized = false
        guard let fb = rdpmcp_frame_lock(h, &w, &ht, &dirty, &resized), w > 0, ht > 0 else {
            rdpmcp_frame_unlock(h)
            return
        }
        let width = Int(w)
        let height = Int(ht)
        if bitmap == nil || width != remoteW || height != remoteH {
            bitmap = CGContext(data: nil, width: width, height: height, bitsPerComponent: 8,
                               bytesPerRow: width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                               bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue
                                   | CGBitmapInfo.byteOrder32Little.rawValue)
            remoteW = width
            remoteH = height
            dirty = rdpmcp_rect(x: 0, y: 0, w: w, h: ht)
            session?.remoteSize = CGSize(width: width, height: height)
        }
        if let bitmap, let dst = bitmap.data, dirty.w > 0, dirty.h > 0 {
            let rowBytes = Int(dirty.w) * 4
            for row in Int(dirty.y) ..< Int(dirty.y + dirty.h) {
                let offset = (row * width + Int(dirty.x)) * 4
                memcpy(dst + offset, fb + offset, rowBytes)
            }
        }
        rdpmcp_frame_unlock(h)
        layer?.contents = bitmap?.makeImage()
    }

    /// Het deel van de view waarin het remote beeld getekend wordt (aspect fit)
    private var contentRect: CGRect {
        guard remoteW > 0, remoteH > 0 else { return bounds }
        let scale = min(bounds.width / CGFloat(remoteW), bounds.height / CGFloat(remoteH))
        let w = CGFloat(remoteW) * scale
        let h = CGFloat(remoteH) * scale
        return CGRect(x: (bounds.width - w) / 2, y: (bounds.height - h) / 2, width: w, height: h)
    }

    private func remotePoint(_ event: NSEvent) -> (Int32, Int32) {
        let p = convert(event.locationInWindow, from: nil)
        let r = contentRect
        guard r.width > 0, r.height > 0 else { return (0, 0) }
        let x = (p.x - r.minX) / r.width * CGFloat(remoteW)
        let y = (p.y - r.minY) / r.height * CGFloat(remoteH)
        return (Int32(max(0, min(CGFloat(remoteW - 1), x))), Int32(max(0, min(CGFloat(remoteH - 1), y))))
    }

    func viewPoint(remoteX x: Int, y: Int) -> CGPoint {
        let r = contentRect
        guard remoteW > 0, remoteH > 0 else { return .zero }
        return CGPoint(x: r.minX + CGFloat(x) / CGFloat(remoteW) * r.width,
                       y: r.minY + CGFloat(y) / CGFloat(remoteH) * r.height)
    }

    func flashAI(x: Int, y: Int) {
        overlay.flash(frame: contentRect, point: x >= 0 ? viewPoint(remoteX: x, y: y) : nil)
    }

    // MARK: - dynamische resolutie

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        scheduleResize()
    }

    private func scheduleResize() {
        guard session?.dynamicResolution == true, bounds.width >= 200, bounds.height >= 200 else { return }
        resizeWork?.cancel()
        let work = DispatchWorkItem { [weak self] in
            guard let self, let h = self.session?.handle else { return }
            rdpmcp_request_resize(h, UInt32(self.bounds.width), UInt32(self.bounds.height))
        }
        resizeWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: work)
    }

    // MARK: - focus

    func focusIfActive() {
        guard isActive else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self, let window = self.window else { return }
            window.makeFirstResponder(self)
        }
    }

    override func becomeFirstResponder() -> Bool {
        if let h = session?.handle {
            let caps = NSEvent.modifierFlags.contains(.capsLock)
            rdpmcp_input_focus(h, caps, false, false)
        }
        return true
    }

    override func resignFirstResponder() -> Bool {
        releaseKeys()
        return true
    }

    private func releaseKeys() {
        if let h = session?.handle { rdpmcp_input_release_all(h) }
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let resignObserver { NotificationCenter.default.removeObserver(resignObserver) }
        resignObserver = nil
        guard let window else { return }
        // Cmd+Tab e.d.: toetsen loslaten, anders blijven ze remote ingedrukt
        resignObserver = NotificationCenter.default.addObserver(
            forName: NSWindow.didResignKeyNotification, object: window, queue: .main
        ) { [weak self] _ in self?.releaseKeys() }
        focusIfActive()
    }

    // MARK: - toetsenbord (via SessionManager)

    func handleKey(_ event: NSEvent) {
        guard let s = session, let h = s.handle else { return }
        switch event.type {
        case .flagsChanged:
            if Int(event.keyCode) == kVK_CapsLock {
                // Windows schakelt bij elke druk; macOS meldt enkel de nieuwe toestand
                rdpmcp_input_key(h, 0x3A, true, false)
                rdpmcp_input_key(h, 0x3A, false, false)
                return
            }
            guard let down = KeyMap.isDown(modifierKey: event.keyCode, flags: event.modifierFlags),
                  let code = KeyMap.scancode(for: event.keyCode, cmdAsWin: cmdAsWin) else { return }
            rdpmcp_input_key(h, code, down, false)

        case .keyDown:
            let flags = event.modifierFlags.intersection(.deviceIndependentFlagsMask)
            if Int(event.keyCode) == kVK_ANSI_A, flags.isSuperset(of: [.control, .option, .shift]) {
                s.setAI(!s.aiEnabled)
                return
            }
            if let code = KeyMap.scancode(for: event.keyCode, cmdAsWin: cmdAsWin) {
                rdpmcp_input_key(h, code, true, event.isARepeat)
            }

        case .keyUp:
            if let code = KeyMap.scancode(for: event.keyCode, cmdAsWin: cmdAsWin) {
                rdpmcp_input_key(h, code, false, false)
            }

        default:
            break
        }
    }

    // MARK: - muis

    private var trackingArea: NSTrackingArea?

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let trackingArea { removeTrackingArea(trackingArea) }
        let area = NSTrackingArea(rect: bounds, options: [.mouseMoved, .activeInKeyWindow, .inVisibleRect],
                                  owner: self)
        addTrackingArea(area)
        trackingArea = area
    }

    private func button(_ event: NSEvent, _ button: rdpmcp_button, down: Bool) {
        guard let h = session?.handle else { return }
        if down, window?.firstResponder !== self { window?.makeFirstResponder(self) }
        let (x, y) = remotePoint(event)
        rdpmcp_input_button(h, button, down, x, y)
    }

    private func move(_ event: NSEvent) {
        guard let h = session?.handle else { return }
        let (x, y) = remotePoint(event)
        rdpmcp_input_move(h, x, y)
    }

    override func mouseDown(with event: NSEvent) { button(event, RDPMCP_BUTTON_LEFT, down: true) }
    override func mouseUp(with event: NSEvent) { button(event, RDPMCP_BUTTON_LEFT, down: false) }
    override func rightMouseDown(with event: NSEvent) { button(event, RDPMCP_BUTTON_RIGHT, down: true) }
    override func rightMouseUp(with event: NSEvent) { button(event, RDPMCP_BUTTON_RIGHT, down: false) }

    private func otherButton(_ event: NSEvent) -> rdpmcp_button? {
        switch event.buttonNumber {
        case 2: return RDPMCP_BUTTON_MIDDLE
        case 3: return RDPMCP_BUTTON_X1
        case 4: return RDPMCP_BUTTON_X2
        default: return nil
        }
    }

    override func otherMouseDown(with event: NSEvent) {
        if let b = otherButton(event) { button(event, b, down: true) }
    }

    override func otherMouseUp(with event: NSEvent) {
        if let b = otherButton(event) { button(event, b, down: false) }
    }

    override func mouseMoved(with event: NSEvent) { move(event) }
    override func mouseDragged(with event: NSEvent) { move(event) }
    override func rightMouseDragged(with event: NSEvent) { move(event) }
    override func otherMouseDragged(with event: NSEvent) { move(event) }

    override func scrollWheel(with event: NSEvent) {
        guard let h = session?.handle else { return }
        let (x, y) = remotePoint(event)
        var dy = 0
        var dx = 0
        if event.hasPreciseScrollingDeltas {
            // trackpad: pixels optellen tot een "klikje" van het wiel
            let step: CGFloat = 24
            scrollAccumY += event.scrollingDeltaY
            scrollAccumX += event.scrollingDeltaX
            dy = Int(scrollAccumY / step)
            dx = Int(scrollAccumX / step)
            scrollAccumY -= CGFloat(dy) * step
            scrollAccumX -= CGFloat(dx) * step
        } else {
            dy = Int(event.scrollingDeltaY.rounded(.awayFromZero))
            dx = Int(event.scrollingDeltaX.rounded(.awayFromZero))
        }
        if dy != 0 { rdpmcp_input_wheel(h, false, Int32(dy), x, y) }
        // macOS: positief = naar links schuiven; RDP: positief = naar rechts
        if dx != 0 { rdpmcp_input_wheel(h, true, Int32(-dx), x, y) }
    }
}

/// Oranje rand en kruisje als de AI iets doet; laat muisklikken door.
final class AIOverlayView: NSView {
    private var contentFrame: CGRect = .zero
    private var point: CGPoint?
    private var hideWork: DispatchWorkItem?

    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    func flash(frame: CGRect, point: CGPoint?) {
        contentFrame = frame
        if let point { self.point = point }
        isHidden = false
        needsDisplay = true
        hideWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.isHidden = true }
        hideWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: work)
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.systemOrange.setStroke()
        NSColor.systemOrange.setFill()
        let border = NSBezierPath(rect: contentFrame.insetBy(dx: 2, dy: 2))
        border.lineWidth = 4
        border.stroke()
        if let p = point {
            NSBezierPath(rect: CGRect(x: p.x - 12, y: p.y - 2, width: 24, height: 4)).fill()
            NSBezierPath(rect: CGRect(x: p.x - 2, y: p.y - 12, width: 4, height: 24)).fill()
        }
    }
}
