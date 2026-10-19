import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var manager: SessionManager
    @EnvironmentObject private var book: AddressBook
    @State private var columns: NavigationSplitViewVisibility = .all
    @State private var editing: HostEntry?

    var body: some View {
        NavigationSplitView(columnVisibility: $columns) {
            AddressBookView(editing: $editing)
                .navigationSplitViewColumnWidth(min: 200, ideal: 250, max: 400)
        } detail: {
            SessionArea()
        }
        .sheet(item: $editing) { entry in
            HostEditor(entry: entry)
        }
        .onChange(of: manager.focusMode) { _, focus in
            withAnimation { columns = focus ? .detailOnly : .all }
        }
        // enkel het hoofdvenster; losgemaakte vensters regelen hun eigen volledig scherm
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.willEnterFullScreenNotification)) { note in
            if (note.object as? NSWindow)?.identifier != DetachedWindow.identifierValue { manager.focusMode = true }
        }
        .onReceive(NotificationCenter.default.publisher(for: NSWindow.willExitFullScreenNotification)) { note in
            if (note.object as? NSWindow)?.identifier != DetachedWindow.identifierValue { manager.focusMode = false }
        }
        .onReceive(NotificationCenter.default.publisher(for: .newHost)) { _ in
            editing = HostEntry()
        }
        .alert("Adresboek", isPresented: Binding(get: { book.loadError != nil },
                                                 set: { if !$0 { book.loadError = nil } })) {
            Button("OK") {}
        } message: {
            Text(book.loadError ?? "")
        }
    }
}

extension Notification.Name {
    static let newHost = Notification.Name("rdpmcp.newHost")
}

/// Tabbalk, sessies en statusbalk
struct SessionArea: View {
    @EnvironmentObject private var manager: SessionManager
    @State private var revealTabs = false

    var body: some View {
        VStack(spacing: 0) {
            if !manager.focusMode {
                TabBar()
                Divider()
            }
            ZStack {
                if manager.attachedSessions.isEmpty {
                    EmptyState(hasDetached: !manager.sessions.isEmpty)
                }
                ForEach(manager.attachedSessions) { session in
                    SessionContainer(session: session, isActive: session.id == manager.selectedID)
                        .opacity(session.id == manager.selectedID ? 1 : 0)
                        .allowsHitTesting(session.id == manager.selectedID)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Color.black)
            .background(GeometryReader { geo in
                Color.clear
                    .onAppear { manager.contentSize = geo.size }
                    .onChange(of: geo.size) { _, size in manager.contentSize = size }
            })
            if !manager.focusMode, let session = manager.selected {
                Divider()
                StatusBar(session: session)
            }
        }
        // Volledig scherm: tabbalk tonen als de muis bovenaan komt
        .overlay(alignment: .top) {
            if manager.focusMode {
                VStack(spacing: 0) {
                    if revealTabs {
                        TabBar().background(.regularMaterial)
                    }
                    Color.clear.frame(height: revealTabs ? 0 : 4).contentShape(Rectangle())
                }
                .onHover { revealTabs = $0 }
                .animation(.easeInOut(duration: 0.15), value: revealTabs)
            }
        }
    }
}

private struct EmptyState: View {
    var hasDetached = false

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "display.2")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text(hasDetached ? "Alle sessies staan in een eigen venster" : "Geen open sessies")
                .font(.title3)
            Text("Dubbelklik een host in het adresboek, of verbind snel via het veld bovenaan.")
                .foregroundStyle(.secondary)
        }
        .padding()
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .windowBackgroundColor))
    }
}

/// Eén sessie: het remote beeld plus een laag tijdens verbinden of na een fout
struct SessionContainer: View {
    @EnvironmentObject private var manager: SessionManager
    @ObservedObject var session: RDPSession
    let isActive: Bool

    var body: some View {
        ZStack {
            RDPViewRepresentable(session: session, isActive: isActive)
            switch session.status {
            case .connected:
                EmptyView()
            case .connecting:
                VStack(spacing: 12) {
                    ProgressView()
                    Text("Verbinden met \(session.entry.displayName)…")
                }
                .padding(24)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
            case .disconnected(let message):
                ended(title: "Verbinding gesloten", message: message)
            case .failed(let message):
                ended(title: "Verbinding mislukt", message: message)
            }
        }
    }

    private func ended(title: String, message: String?) -> some View {
        VStack(spacing: 12) {
            Image(systemName: "bolt.horizontal.circle")
                .font(.system(size: 36))
            Text(title)
                .font(.title3)
            if let message, !message.isEmpty {
                Text(message)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
                    .frame(maxWidth: 420)
            }
            HStack {
                Button("Opnieuw verbinden") {
                    Task { await manager.reconnect(session) }
                }
                .keyboardShortcut(.defaultAction)
                Button("Tab sluiten") { manager.close(session) }
            }
        }
        .padding(24)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
    }
}

/// Plaatst de (enige) RDPView van een sessie in een eigen container. Bij het verhuizen
/// naar een ander venster neemt de nieuwe container de view over; de oude ruimt enkel
/// op wat nog van hem is, zodat hij de view niet uit het nieuwe venster haalt.
struct RDPViewRepresentable: NSViewRepresentable {
    let session: RDPSession
    let isActive: Bool

    func makeNSView(context: Context) -> NSView {
        let container = NSView()
        let view = session.view
        view.removeFromSuperview()
        view.frame = container.bounds
        view.autoresizingMask = [.width, .height]
        container.addSubview(view)
        return container
    }

    func updateNSView(_ container: NSView, context: Context) {
        let view = session.view
        if view.superview === container, view.isActive != isActive { view.isActive = isActive }
    }

    static func dismantleNSView(_ container: NSView, coordinator: ()) {
        for sub in container.subviews { sub.removeFromSuperview() }
    }
}
