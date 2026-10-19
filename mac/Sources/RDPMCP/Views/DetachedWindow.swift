import AppKit
import SwiftUI

/// Eigen venster voor één losgemaakte sessie. Sluiten zet de sessie terug als tab.
final class DetachedWindow: NSWindow, NSWindowDelegate {
    static let identifierValue = NSUserInterfaceItemIdentifier("rdpmcp.detached")
    var onClose: (() -> Void)?
    /// Het venster is al aan het sluiten; niet nog eens close() oproepen
    private(set) var isClosing = false

    init(session: RDPSession, manager: SessionManager, book: AddressBook, size: CGSize) {
        super.init(contentRect: NSRect(origin: .zero, size: size),
                   styleMask: [.titled, .closable, .miniaturizable, .resizable],
                   backing: .buffered, defer: false)
        identifier = Self.identifierValue
        title = session.entry.displayName
        isReleasedWhenClosed = false
        collectionBehavior.insert(.fullScreenPrimary)
        tabbingMode = .disallowed
        delegate = self
        contentView = NSHostingView(rootView: DetachedSessionView(session: session)
            .environmentObject(manager)
            .environmentObject(book))
        setFrameAutosaveName("rdpmcp.detached.\(session.entry.id.uuidString)")
        if frame.origin == .zero { center() }
    }

    func windowWillClose(_ notification: Notification) {
        isClosing = true
        let close = onClose
        onClose = nil
        close?()
    }
}

private struct DetachedSessionView: View {
    @EnvironmentObject private var manager: SessionManager
    @ObservedObject var session: RDPSession
    @State private var fullScreen = false

    var body: some View {
        VStack(spacing: 0) {
            SessionContainer(session: session, isActive: true)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .background(Color.black)
            if !fullScreen {
                Divider()
                StatusBar(session: session, detached: true)
            }
        }
        .frame(minWidth: 640, minHeight: 400)
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.willEnterFullScreenNotification)) { note in
            if isMyWindow(note) { fullScreen = true }
        }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.willExitFullScreenNotification)) { note in
            if isMyWindow(note) { fullScreen = false }
        }
    }

    private func isMyWindow(_ note: Notification) -> Bool {
        (note.object as? NSWindow) === session.view.window
    }
}
