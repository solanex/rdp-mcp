import CoreGraphics
import Foundation
import Vision

/// Herkent op het scherm of een klik of Enter van de AI Windows zou herstarten,
/// afsluiten of afmelden. Leest de tekst met Vision (lokaal op de Mac).
///
/// Dit is een vangnet tegen vergissingen, geen harde grens: een knop zonder tekst,
/// een andere taal of een ongewoon thema glipt erdoor. Echt tegenhouden kan alleen
/// Windows zelf (rechten van het account, GPO).
enum PowerGuard {
    /// Wat de AI wil doen
    enum Action {
        case click(x: Int, y: Int)
        /// Enter (of spatie: enter = false). Een spatie voert geen commando uit.
        case key(enter: Bool)
    }

    struct Finding {
        /// De herkende tekst die de vraag uitlokte, zoals op het scherm
        let text: String
        /// Plaats van die tekst in remote pixels (oorsprong linksboven), nil = geen vaste plek
        let box: CGRect?
        /// Gezet als het om een commando gaat: wat het doet (CommandGuard)
        var commandReason: String? = nil
    }

    /// Herkende teksten langer dan dit zijn zinnen of code, geen knop of menu-item
    private static let maxLabelLength = 40

    /// nil = niets gevaarlijks herkend. cache: wat eerder op dit scherm gelezen werd
    static func check(_ action: Action, image: CGImage, cache: ScreenCache) -> Finding? {
        let rules = GuardSettings.current
        let powerPhrases = rules.clickWords.map(normalize).filter { !$0.isEmpty }
        let ambiguousPhrases = rules.contextWords.map(normalize).filter { !$0.isEmpty }
        let dialogPhrases = rules.dialogTitles.map(normalize).filter { !$0.isEmpty }
        let cancelPhrases = rules.cancelWords.map(normalize).filter { !$0.isEmpty }

        // Altijd het nauwkeurige niveau: het snelle las op een echt scherm het
        // aan/uit-menu als "OpnièLtbYOPSiartèn", en Enter ging door
        var lines = screenLines(image, cache: cache)
        if case .click(let x, let y) = action {
            let area = CGRect(x: x - 400, y: y - 120, width: 800, height: 240)
                .intersection(CGRect(x: 0, y: 0, width: image.width, height: image.height)).integral
            // vooraan: bij een treffer toont de dialoog liefst de nauwkeurige lezing
            lines = recognize(image, level: .accurate, region: area) + lines
        }
        let dialog = lines.first { line in dialogPhrases.contains { contains(line.text, $0) } }
        let powerVisible = Set(lines.flatMap { line in
            (powerPhrases + ambiguousPhrases).filter { line.text.count <= maxLabelLength && contains(line.text, $0) }
        }).count >= 2

        switch action {
        case .key(let enter):
            // Enter of spatie bevestigt het geselecteerde item van een open menu of venster
            if let dialog { return Finding(text: dialog.original, box: dialog.box) }
            if powerVisible { return Finding(text: String(localized: "power menu"), box: nil) }
            // Enter voert ook het commando uit dat in een console of in Uitvoeren staat,
            // ook als het uit de geschiedenis of het klembord kwam
            return enter ? commandFinding(lines, image: image) : nil

        case .click(let x, let y):
            let point = CGPoint(x: x, y: y)
            // het icoon staat links van de tekst, dus links ruimer zoeken
            let hits = lines.filter { line in
                let b = line.box
                return CGRect(x: b.minX - 60, y: b.minY - 6, width: b.width + 68, height: b.height + 12).contains(point)
            }
            for line in hits where line.text.count <= maxLabelLength {
                if powerPhrases.contains(where: { contains(line.text, $0) }) {
                    return Finding(text: line.original, box: line.box)
                }
                if (dialog != nil || powerVisible), ambiguousPhrases.contains(where: { contains(line.text, $0) }) {
                    return Finding(text: line.original, box: line.box)
                }
            }
            // In het afsluitvenster doet "OK" het werk; enkel Annuleren mag zonder vragen
            if let dialog, !hits.contains(where: { line in cancelPhrases.contains { contains(line.text, $0) } }) {
                return Finding(text: dialog.original, box: dialog.box)
            }
            return nil
        }
    }

    // MARK: - commando's op het scherm

    /// Prompt van PowerShell of cmd: "PS C:\Users\x> " of "C:\Windows>". Vision leest
    /// die niet altijd letterlijk: "ps c: \Users", "C:lusers", "»" of "›" voor ">".
    /// Daarom: stationsletter, dubbelpunt, en het pad tot het eerste ">".
    private static let prompt = try! NSRegularExpression(
        pattern: #"^(?:ps\s+)?[a-z]\s*:\s*[^>\s][^>]*>\s*(.*)$"#, options: [.caseInsensitive])
    /// Label van het venster Uitvoeren (Win+R)
    private static let runLabels = ["openen", "open", "ouvrir"]

    /// De onderste prompt-regel (daar typt de console) en het veld van Uitvoeren
    fileprivate static func commandFinding(_ lines: [Line], image: CGImage) -> Finding? {
        // Het snelle niveau verminkt paden ("PS C\'.IUsersladmin>"): regels met ">"
        // van onder naar boven nauwkeurig herlezen; de eerste echte prompt is de
        // regel waar de console typt.
        let candidates = lines.filter { line in [">", "»", "›"].contains { line.original.contains($0) } }
            .sorted { $0.box.midY > $1.box.midY }
            .prefix(3)
        for candidate in candidates {
            let reread = reread(candidate, in: image)
            if let line = reread.first(where: { matchesPrompt($0.original) }) {
                let command = commandText(line.original)
                if let reason = CommandGuard.check(command) {
                    return Finding(text: command, box: line.box, commandReason: reason)
                }
                // Niet stoppen: een console op de achtergrond met een lege prompt mag
                // het venster Uitvoeren ervoor niet verbergen
                break
            }
        }
        // Uitvoeren: het invoerveld staat rechts van het label, op dezelfde hoogte
        for label in lines {
            // soms leest Vision label en veld als één regel: "Openen: shutdown /r"
            let head = label.text.split(separator: ":", maxSplits: 1).map(String.init)
            let letters = (head.first ?? "").filter { $0.isLetter }
            guard letters.count <= 7, runLabels.contains(where: { $0 == letters || ($0.count >= 6 && distance(letters, $0) <= 1) })
            else { continue }
            if head.count == 2, let colon = label.original.firstIndex(of: ":") {
                let rest = label.original[label.original.index(after: colon)...].trimmingCharacters(in: .whitespaces)
                if let reason = CommandGuard.check(rest) {
                    return Finding(text: rest, box: label.box, commandReason: reason)
                }
            }
            let fields = lines.filter { $0.box.minX > label.box.maxX && abs($0.box.midY - label.box.midY) < label.box.height }
            for field in fields {
                for line in reread(field, in: image) + [field] where line.box.minX > label.box.maxX - 4 {
                    if let reason = CommandGuard.check(line.original) {
                        return Finding(text: line.original, box: line.box, commandReason: reason)
                    }
                }
            }
        }
        return nil
    }

    /// Het gebied rond de regel opnieuw lezen met het nauwkeurige niveau. Niet als
    /// smalle strook over de hele breedte: daarin vindt Vision niets.
    fileprivate static func reread(_ line: Line, in image: CGImage) -> [Line] {
        let width = max(line.box.width + 200, 600)
        let height = max(line.box.height * 6, 120)
        let strip = CGRect(x: line.box.minX - 100, y: line.box.midY - height / 2, width: width, height: height)
            .intersection(CGRect(x: 0, y: 0, width: image.width, height: image.height)).integral
        return recognize(image, level: .accurate, region: strip)
            .filter { abs($0.box.midY - line.box.midY) < line.box.height }
    }

    private static func matchesPrompt(_ text: String) -> Bool {
        let text = promptForm(text)
        return prompt.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }

    /// Wat Vision voor ">" leest, terugzetten
    private static func promptForm(_ text: String) -> String {
        text.replacingOccurrences(of: "»", with: ">").replacingOccurrences(of: "›", with: ">")
            .replacingOccurrences(of: "〉", with: ">")
    }

    /// De tekst na de prompt, of de hele regel als er geen prompt in staat
    private static func commandText(_ line: String) -> String {
        let line = promptForm(line)
        let range = NSRange(line.startIndex..., in: line)
        if let m = prompt.firstMatch(in: line, range: range), let r = Range(m.range(at: 1), in: line) {
            return String(line[r])
        }
        return line
    }

    // MARK: - het hele scherm lezen

    private static let thumbScale = 4

    /// Alle tekst op het scherm. Leest enkel het deel dat veranderde sinds de vorige
    /// keer (bij typen een paar tekens); de rest komt uit de cache.
    fileprivate static func screenLines(_ image: CGImage, cache: ScreenCache) -> [Line] {
        let thumb = thumbnail(image)
        let tw = max(1, image.width / thumbScale), th = max(1, image.height / thumbScale)
        let full = CGRect(x: 0, y: 0, width: image.width, height: image.height)
        cache.lock.lock()
        defer { cache.lock.unlock() }

        var region = full
        if cache.size == (image.width, image.height), cache.thumb.count == thumb.count {
            var minX = tw, minY = th, maxX = -1, maxY = -1
            for y in 0..<th {
                for x in 0..<tw where abs(Int(thumb[y * tw + x]) - Int(cache.thumb[y * tw + x])) > 8 {
                    minX = min(minX, x); maxX = max(maxX, x)
                    minY = min(minY, y); maxY = max(maxY, y)
                }
            }
            if maxX < 0 { return cache.lines } // niets veranderd
            let s = CGFloat(thumbScale)
            let changed = CGRect(x: CGFloat(minX) * s, y: CGFloat(minY) * s,
                                 width: CGFloat(maxX - minX + 1) * s, height: CGFloat(maxY - minY + 1) * s)
            // ruim genoeg dat een regel die net veranderde heel gelezen wordt
            let grown = changed.insetBy(dx: -max(40, (600 - changed.width) / 2), dy: -max(30, (160 - changed.height) / 2))
                .intersection(full).integral
            if grown.width * grown.height < full.width * full.height * 0.4 { region = grown }
        }

        let fresh = recognizeTiled(image, region: region)
        let lines = region == full ? fresh : cache.lines.filter { !$0.box.intersects(region) } + fresh
        cache.thumb = thumb
        cache.size = (image.width, image.height)
        cache.lines = lines
        return lines
    }

    /// Het nauwkeurige niveau vindt niets op een volledig scherm of een smalle strook:
    /// lezen in tegels van hoogstens 1000 × 600, die elkaar overlappen
    fileprivate static func recognizeTiled(_ image: CGImage, region: CGRect) -> [Line] {
        let overlap: CGFloat = 40
        let cols = Int((region.width / min(1000, max(region.height, 200) * 3)).rounded(.up))
        let rows = Int((region.height / 600).rounded(.up))
        let w = region.width / CGFloat(max(cols, 1)), h = region.height / CGFloat(max(rows, 1))
        var lines: [Line] = []
        for row in 0..<max(rows, 1) {
            for col in 0..<max(cols, 1) {
                let tile = CGRect(x: region.minX + CGFloat(col) * w - overlap, y: region.minY + CGFloat(row) * h - overlap,
                                  width: w + 2 * overlap, height: h + 2 * overlap)
                    .intersection(region).integral
                lines += recognize(image, level: .accurate, region: tile)
            }
        }
        return lines
    }

    /// Verkleind grijswaardenbeeld, om veranderingen te vinden
    private static func thumbnail(_ image: CGImage) -> [UInt8] {
        let w = max(1, image.width / thumbScale), h = max(1, image.height / thumbScale)
        var pixels = [UInt8](repeating: 0, count: w * h)
        pixels.withUnsafeMutableBytes { buf in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                                      space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGImageAlphaInfo.none.rawValue)
            else { return }
            ctx.interpolationQuality = .medium
            ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        }
        return pixels
    }

    // MARK: - tekstherkenning

    fileprivate struct Line {
        let original: String
        /// kleine letters, zonder accenten, met enkele spaties
        let text: String
        let box: CGRect
    }

    /// region: enkel dit deel lezen (remote pixels); de vakken zijn altijd in schermcoördinaten
    fileprivate static func recognize(_ full: CGImage, level: VNRequestTextRecognitionLevel,
                                  region: CGRect? = nil) -> [Line] {
        let offset = region?.origin ?? .zero
        guard let image = region.map({ full.cropping(to: $0) }) ?? full else { return [] }
        let request = VNRecognizeTextRequest()
        request.recognitionLevel = level
        request.usesLanguageCorrection = false
        request.recognitionLanguages = ["nl-NL", "en-US", "fr-FR"]
        do {
            try VNImageRequestHandler(cgImage: image).perform([request])
        } catch {
            NSLog("rdp-mcp: text recognition failed (%@)", error.localizedDescription)
            return []
        }
        let w = CGFloat(image.width), h = CGFloat(image.height)
        return (request.results ?? []).compactMap { obs in
            guard let s = obs.topCandidates(1).first?.string else { return nil }
            // Vision: genormaliseerd, oorsprong linksonder
            let b = obs.boundingBox
            let box = CGRect(x: offset.x + b.minX * w, y: offset.y + (1 - b.maxY) * h,
                             width: b.width * w, height: b.height * h)
            return Line(original: s, text: normalize(s), box: box)
        }
    }

    static func normalize(_ s: String) -> String {
        s.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil)
            .split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    /// Of `phrase` als woord(en) in `text` staat, met ruimte voor een leesfout per
    /// woord van 6 letters of meer (Vision leest soms "opstsrten")
    static func contains(_ text: String, _ phrase: String) -> Bool {
        let words = text.split(separator: " ").map { String($0.filter { $0.isLetter }) }
        let want = phrase.split(separator: " ").map(String.init)
        guard words.count >= want.count else { return false }
        for start in 0...(words.count - want.count) {
            let ok = zip(words[start..<start + want.count], want).allSatisfy { got, w in
                got == w || (w.count >= 6 && distance(got, w) <= 1)
            }
            if ok { return true }
        }
        return false
    }

    /// Levenshtein-afstand
    private static func distance(_ a: String, _ b: String) -> Int {
        let a = Array(a), b = Array(b)
        var prev = Array(0...b.count)
        for i in a.indices {
            var cur = [i + 1]
            for j in b.indices {
                cur.append(min(prev[j + 1] + 1, cur[j] + 1, prev[j] + (a[i] == b[j] ? 0 : 1)))
            }
            prev = cur
        }
        return prev[b.count]
    }

    /// BGRX32 van de kern naar een CGImage (kopieert)
    static func image(bgrx: UnsafePointer<UInt8>, width: Int, height: Int) -> CGImage? {
        let data = Data(bytes: bgrx, count: width * height * 4)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: width * 4, space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue
                           | CGImageAlphaInfo.noneSkipFirst.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
    }
}

/// Wat de app laatst op het scherm van een sessie las (PowerGuard)
final class ScreenCache {
    fileprivate let lock = NSLock()
    fileprivate var thumb: [UInt8] = []
    fileprivate var size = (0, 0)
    fileprivate var lines: [PowerGuard.Line] = []
}
