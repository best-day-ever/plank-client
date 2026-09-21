// plank-passkey: Secure Enclave passkey helper for the PLANK Client's
// Remote (broker) mode. Contract: bde-linux docs/plank-broker.md section 13
// (13.1 signature format verified by FreeIPA, 13.4 helper interface).
//
// The key is a CryptoKit Secure Enclave P-256 key that requires user
// presence (Touch ID or the Mac password) for every signature. Only its
// Secure Enclave-wrapped dataRepresentation is stored, owner-only, under
//   ~/Library/Application Support/PLANK/passkeys/<rp_id>/<username>/<credential-id-hex>.json
// That blob is useless on any other Mac and needs no keychain entitlement.
//
// Subcommands (JSON on stdout, diagnostics on stderr):
//   create --rp <rp_id> --user <name>   -> {"credential_id","public_key","mapping"}
//   assert --rp <rp_id> --user <name>   stdin {"rp_id","credential_ids","challenge","user_verification"}
//                                       -> {"credential_id","authenticator_data","signature"}
//   list [--rp <rp_id>] [--user <name>] -> [{"rp_id","username","credential_id","public_key","mapping","created"}]
//   delete --rp <rp_id> --user <name> [--credential-id <b64>] -> {"deleted":n}
//   self-test                           stdin as for assert; signs with a throwaway software key through
//                                       the same code path -> assert output plus "public_key"
// Exit codes: 0 ok, 1 failure, 2 invalid input, 3 no matching local key,
//             4 user presence not confirmed (cancelled or failed).
//
// Signature format (13.1): authData = SHA-256(rp_id) || 0x05 (UP|UV) || counter 00 00 00 00,
// signature = DER ECDSA-P256-SHA256(authData || challenge), challenge = the raw 32 bytes.
// The broker, not this helper, CBOR-wraps authData for the KDC. Base64 is standard with padding.

import CryptoKit
import Foundation
import LocalAuthentication
import Security

enum ExitCode: Int32 {
    case ok = 0
    case failure = 1
    case invalidInput = 2
    case noMatchingKey = 3
    case notConfirmed = 4
}

struct HelperError: Error {
    let code: ExitCode
    let message: String
    init(_ code: ExitCode, _ message: String) {
        self.code = code
        self.message = message
    }
}

let maximumInputBytes = 64 * 1024
let maximumCredentialIds = 64
let maximumCredentialIdBytes = 1024
let challengeBytes = 32
let credentialIdBytes = 32

// MARK: - Validation

// A plain DNS host name: lower-case LDH labels, at least one dot-free
// alphabetic top label (so IP literals are refused), at most 253 characters.
func isPlainHostname(_ value: String) -> Bool {
    guard !value.isEmpty, value.utf8.count <= 253 else { return false }
    let labels = value.split(separator: ".", omittingEmptySubsequences: false)
    for label in labels {
        let bytes = Array(label.utf8)
        guard (1...63).contains(bytes.count) else { return false }
        for (index, byte) in bytes.enumerated() {
            let alnum = (byte >= 0x61 && byte <= 0x7a) || (byte >= 0x30 && byte <= 0x39)
            let hyphen = byte == 0x2d && index != 0 && index != bytes.count - 1
            guard alnum || hyphen else { return false }
        }
    }
    return labels.last!.utf8.contains { $0 >= 0x61 && $0 <= 0x7a }
}

// FreeIPA user names are lower case; the name is also a directory name,
// so '/', leading dots and anything outside [a-z0-9_.-] are refused.
func isUsername(_ value: String) -> Bool {
    let bytes = Array(value.utf8)
    guard (1...64).contains(bytes.count) else { return false }
    for (index, byte) in bytes.enumerated() {
        let lower = byte >= 0x61 && byte <= 0x7a
        let digit = byte >= 0x30 && byte <= 0x39
        let punctuation = byte == 0x5f || ((byte == 0x2e || byte == 0x2d) && index != 0)
        guard lower || digit || punctuation else { return false }
    }
    return true
}

func decodeBase64(_ value: String) -> Data? {
    // Foundation's decoder is strict standard base64 (padding required, no whitespace).
    guard let data = Data(base64Encoded: value), data.base64EncodedString() == value else { return nil }
    return data
}

func hex(_ data: Data) -> String {
    data.map { String(format: "%02x", $0) }.joined()
}

// MARK: - Assertion (shared by the Secure Enclave and self-test paths)

struct AssertionRequest {
    let rpId: String
    let credentialIds: [Data]
    let challenge: Data
    let userVerification: Bool
}

func parseAssertionRequest(_ input: Data) throws -> AssertionRequest {
    guard input.count <= maximumInputBytes,
          let object = try? JSONSerialization.jsonObject(with: input),
          let dictionary = object as? [String: Any] else {
        throw HelperError(.invalidInput, "stdin must be the broker's passkey prompt object")
    }
    guard let rpId = dictionary["rp_id"] as? String, isPlainHostname(rpId) else {
        throw HelperError(.invalidInput, "rp_id must be a plain host name")
    }
    guard let challengeText = dictionary["challenge"] as? String,
          let challenge = decodeBase64(challengeText), challenge.count == challengeBytes else {
        throw HelperError(.invalidInput, "challenge must be exactly \(challengeBytes) bytes of standard base64")
    }
    guard let idTexts = dictionary["credential_ids"] as? [Any], !idTexts.isEmpty,
          idTexts.count <= maximumCredentialIds else {
        throw HelperError(.invalidInput, "credential_ids must be a non-empty list")
    }
    var credentialIds: [Data] = []
    for value in idTexts {
        guard let text = value as? String, let id = decodeBase64(text),
              !id.isEmpty, id.count <= maximumCredentialIdBytes else {
            throw HelperError(.invalidInput, "credential_ids must be standard base64 strings")
        }
        credentialIds.append(id)
    }
    var userVerification = true
    if let value = dictionary["user_verification"] {
        guard let flag = value as? Bool else {
            throw HelperError(.invalidInput, "user_verification must be a boolean")
        }
        userVerification = flag
    }
    return AssertionRequest(rpId: rpId, credentialIds: credentialIds, challenge: challenge,
                            userVerification: userVerification)
}

// SHA-256(rp_id) || flags UP|UV || sign counter 0 (big endian).
func authenticatorData(rpId: String) -> Data {
    var data = Data(SHA256.hash(data: Data(rpId.utf8)))
    data.append(contentsOf: [0x05, 0x00, 0x00, 0x00, 0x00])
    return data
}

struct Assertion {
    let credentialId: Data
    let authenticatorData: Data
    let signature: Data

    var json: [String: Any] {
        [
            "credential_id": credentialId.base64EncodedString(),
            "authenticator_data": authenticatorData.base64EncodedString(),
            "signature": signature.base64EncodedString(),
        ]
    }
}

// `sign` receives the message authData || challenge and returns a DER
// ECDSA signature; CryptoKit's signature(for:) hashes it with SHA-256.
func makeAssertion(request: AssertionRequest, credentialId: Data, publicKey: P256.Signing.PublicKey,
                   sign: (Data) throws -> P256.Signing.ECDSASignature) throws -> Assertion {
    let authData = authenticatorData(rpId: request.rpId)
    let message = authData + request.challenge
    let signature = try sign(message)
    // Never hand out a signature the verifier would refuse.
    guard publicKey.isValidSignature(signature, for: message) else {
        throw HelperError(.failure, "signature self-check failed")
    }
    return Assertion(credentialId: credentialId, authenticatorData: authData,
                     signature: signature.derRepresentation)
}

func mapping(credentialId: Data, publicKey: P256.Signing.PublicKey) -> String {
    "passkey:\(credentialId.base64EncodedString()),\(publicKey.derRepresentation.base64EncodedString())"
}

// MARK: - Store

struct StoredKey: Codable {
    var version: Int
    var rp_id: String
    var username: String
    var credential_id: String
    var public_key: String
    var key: String
    var created: String
}

func storeRoot() throws -> URL {
    if let override = ProcessInfo.processInfo.environment["PLANK_PASSKEY_STORE"], !override.isEmpty {
        guard override.hasPrefix("/") else { throw HelperError(.invalidInput, "PLANK_PASSKEY_STORE must be absolute") }
        return URL(fileURLWithPath: override, isDirectory: true)
    }
    guard let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
        throw HelperError(.failure, "no Application Support directory")
    }
    return support.appendingPathComponent("PLANK", isDirectory: true)
        .appendingPathComponent("passkeys", isDirectory: true)
}

// Creates each missing level owner-only and tightens the levels we own.
func ensurePrivateDirectory(_ url: URL, ownedLevels: [URL]) throws {
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: true,
                                            attributes: [.posixPermissions: 0o700])
    for level in ownedLevels where chmod(level.path, 0o700) != 0 {
        throw HelperError(.failure, "cannot restrict \(level.lastPathComponent)")
    }
}

func isOwnerOnlyFile(_ path: String) -> Bool {
    var info = stat()
    guard lstat(path, &info) == 0 else { return false }
    return (info.st_mode & S_IFMT) == S_IFREG && info.st_uid == getuid() && (info.st_mode & 0o077) == 0
}

func loadKeys(root: URL, rpId: String?, username: String?) -> [(URL, StoredKey)] {
    let manager = FileManager.default
    func children(_ url: URL) -> [URL] {
        ((try? manager.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)) ?? [])
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }
    var keys: [(URL, StoredKey)] = []
    let rpDirectories = rpId.map { [root.appendingPathComponent($0, isDirectory: true)] } ?? children(root)
    for rpDirectory in rpDirectories where isPlainHostname(rpDirectory.lastPathComponent) {
        let userDirectories = username.map { [rpDirectory.appendingPathComponent($0, isDirectory: true)] } ??
            children(rpDirectory)
        for userDirectory in userDirectories where isUsername(userDirectory.lastPathComponent) {
            for file in children(userDirectory) where file.pathExtension == "json" {
                guard isOwnerOnlyFile(file.path), let data = try? Data(contentsOf: file),
                      let key = try? JSONDecoder().decode(StoredKey.self, from: data),
                      key.version == 1, key.rp_id == rpDirectory.lastPathComponent,
                      key.username == userDirectory.lastPathComponent,
                      let id = decodeBase64(key.credential_id),
                      file.lastPathComponent == hex(id) + ".json" else {
                    continue
                }
                keys.append((file, key))
            }
        }
    }
    return keys
}

// MARK: - Commands

func requireOptions(_ options: [String: String], _ names: [String]) throws {
    for name in names where options[name] == nil {
        throw HelperError(.invalidInput, "--\(name) is required")
    }
}

func validated(_ options: [String: String]) throws -> (rp: String?, user: String?) {
    if let rp = options["rp"], !isPlainHostname(rp) {
        throw HelperError(.invalidInput, "--rp must be a plain host name")
    }
    if let user = options["user"], !isUsername(user) {
        throw HelperError(.invalidInput, "--user must be a lower-case user name")
    }
    return (options["rp"], options["user"])
}

func create(rpId: String, username: String) throws -> [String: Any] {
    guard SecureEnclave.isAvailable else {
        throw HelperError(.failure, "this Mac has no Secure Enclave")
    }
    var error: Unmanaged<CFError>?
    guard let access = SecAccessControlCreateWithFlags(kCFAllocatorDefault,
                                                       kSecAttrAccessibleWhenUnlockedThisDeviceOnly,
                                                       [.privateKeyUsage, .userPresence], &error) else {
        throw HelperError(.failure, "cannot create the access control: \(String(describing: error?.takeRetainedValue()))")
    }
    let key: SecureEnclave.P256.Signing.PrivateKey
    do {
        key = try SecureEnclave.P256.Signing.PrivateKey(accessControl: access)
    } catch {
        throw HelperError(.failure, "Secure Enclave key creation failed: \(error)")
    }
    var idBytes = [UInt8](repeating: 0, count: credentialIdBytes)
    guard SecRandomCopyBytes(kSecRandomDefault, idBytes.count, &idBytes) == errSecSuccess else {
        throw HelperError(.failure, "no randomness for the credential id")
    }
    let credentialId = Data(idBytes)
    let root = try storeRoot()
    let rpDirectory = root.appendingPathComponent(rpId, isDirectory: true)
    let userDirectory = rpDirectory.appendingPathComponent(username, isDirectory: true)
    try ensurePrivateDirectory(userDirectory, ownedLevels: [root, rpDirectory, userDirectory])

    let formatter = ISO8601DateFormatter()
    let stored = StoredKey(version: 1, rp_id: rpId, username: username,
                           credential_id: credentialId.base64EncodedString(),
                           public_key: key.publicKey.derRepresentation.base64EncodedString(),
                           key: key.dataRepresentation.base64EncodedString(),
                           created: formatter.string(from: Date()))
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    let data = try encoder.encode(stored)
    let final = userDirectory.appendingPathComponent(hex(credentialId) + ".json")
    let temporary = userDirectory.appendingPathComponent(".\(hex(credentialId)).tmp")
    let descriptor = open(temporary.path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0o600)
    guard descriptor >= 0 else { throw HelperError(.failure, "cannot write the key file") }
    let written = data.withUnsafeBytes { write(descriptor, $0.baseAddress, $0.count) }
    let synced = fsync(descriptor) == 0
    close(descriptor)
    guard written == data.count, synced, rename(temporary.path, final.path) == 0 else {
        unlink(temporary.path)
        throw HelperError(.failure, "cannot write the key file")
    }
    return [
        "credential_id": credentialId.base64EncodedString(),
        "public_key": stored.public_key,
        "mapping": mapping(credentialId: credentialId, publicKey: key.publicKey),
    ]
}

func readStandardInput() throws -> Data {
    var input = Data()
    while true {
        let chunk = FileHandle.standardInput.readData(ofLength: 16 * 1024)
        if chunk.isEmpty { break }
        input.append(chunk)
        if input.count > maximumInputBytes {
            throw HelperError(.invalidInput, "stdin is too large")
        }
    }
    return input
}

func assertWithSecureEnclave(rpId: String, username: String) throws -> [String: Any] {
    let request = try parseAssertionRequest(try readStandardInput())
    guard request.rpId == rpId else {
        throw HelperError(.noMatchingKey, "the prompt is for another relying party")
    }
    let keys = loadKeys(root: try storeRoot(), rpId: rpId, username: username)
    // First allowed credential (broker order) that exists on this Mac.
    var selected: (Data, StoredKey)?
    for id in request.credentialIds {
        if let match = keys.first(where: { decodeBase64($0.1.credential_id) == id }) {
            selected = (id, match.1)
            break
        }
    }
    guard let (credentialId, stored) = selected else {
        throw HelperError(.noMatchingKey, "no local passkey matches the allowed credentials")
    }
    guard let blob = decodeBase64(stored.key), let publicDer = decodeBase64(stored.public_key),
          let publicKey = try? P256.Signing.PublicKey(derRepresentation: publicDer) else {
        throw HelperError(.failure, "the local key file is damaged")
    }
    let context = LAContext()
    context.localizedReason = "Sign in to PLANK remote as \(username)"
    let key: SecureEnclave.P256.Signing.PrivateKey
    do {
        key = try SecureEnclave.P256.Signing.PrivateKey(dataRepresentation: blob, authenticationContext: context)
    } catch {
        throw HelperError(.failure, "the local key cannot be used on this Mac: \(error)")
    }
    guard key.publicKey.derRepresentation == publicDer else {
        throw HelperError(.failure, "the local key file is inconsistent")
    }
    do {
        return try makeAssertion(request: request, credentialId: credentialId, publicKey: publicKey) {
            try key.signature(for: $0)
        }.json
    } catch let error as HelperError {
        throw error
    } catch {
        throw HelperError(.notConfirmed, "Touch ID was not confirmed: \(error)")
    }
}

// Same request parsing and assertion code as `assert`, with a throwaway
// software key, so the byte format can be checked without Touch ID.
func selfTest() throws -> [String: Any] {
    let request = try parseAssertionRequest(try readStandardInput())
    let key = P256.Signing.PrivateKey()
    var output = try makeAssertion(request: request, credentialId: request.credentialIds[0],
                                   publicKey: key.publicKey) {
        try key.signature(for: $0)
    }.json
    output["public_key"] = key.publicKey.derRepresentation.base64EncodedString()
    output["mapping"] = mapping(credentialId: request.credentialIds[0], publicKey: key.publicKey)
    return output
}

func list(rpId: String?, username: String?) throws -> [[String: Any]] {
    let root = try storeRoot()
    return loadKeys(root: root, rpId: rpId, username: username).compactMap { _, key in
        guard let id = decodeBase64(key.credential_id), let der = decodeBase64(key.public_key),
              let publicKey = try? P256.Signing.PublicKey(derRepresentation: der) else { return nil }
        return [
            "rp_id": key.rp_id,
            "username": key.username,
            "credential_id": key.credential_id,
            "public_key": key.public_key,
            "mapping": mapping(credentialId: id, publicKey: publicKey),
            "created": key.created,
        ]
    }
}

func delete(rpId: String, username: String, credentialId: String?) throws -> [String: Any] {
    if let credentialId, decodeBase64(credentialId) == nil {
        throw HelperError(.invalidInput, "--credential-id must be standard base64")
    }
    let root = try storeRoot()
    var deleted = 0
    for (file, key) in loadKeys(root: root, rpId: rpId, username: username)
    where credentialId == nil || key.credential_id == credentialId {
        guard unlink(file.path) == 0 else { throw HelperError(.failure, "cannot delete a key file") }
        deleted += 1
    }
    // Leave no empty per-user directory behind (ignored if other files remain).
    rmdir(root.appendingPathComponent(rpId).appendingPathComponent(username).path)
    guard deleted > 0 else { throw HelperError(.noMatchingKey, "no local passkey to delete") }
    return ["deleted": deleted]
}

// MARK: - Main

func emit(_ object: Any) throws {
    var data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])
    data.append(0x0a)
    FileHandle.standardOutput.write(data)
}

func parseOptions(_ arguments: ArraySlice<String>, allowed: Set<String>) throws -> [String: String] {
    var options: [String: String] = [:]
    var iterator = arguments.makeIterator()
    while let argument = iterator.next() {
        guard argument.hasPrefix("--"), allowed.contains(String(argument.dropFirst(2))),
              options[String(argument.dropFirst(2))] == nil, let value = iterator.next() else {
            throw HelperError(.invalidInput, "unexpected argument \(argument)")
        }
        options[String(argument.dropFirst(2))] = value
    }
    return options
}

let usage = """
usage: plank-passkey create --rp <rp_id> --user <name>
       plank-passkey assert --rp <rp_id> --user <name>   (stdin: passkey prompt JSON)
       plank-passkey list [--rp <rp_id>] [--user <name>]
       plank-passkey delete --rp <rp_id> --user <name> [--credential-id <b64>]
       plank-passkey self-test                           (stdin: passkey prompt JSON)
"""

do {
    let arguments = CommandLine.arguments
    guard arguments.count >= 2 else { throw HelperError(.invalidInput, usage) }
    let rest = arguments.dropFirst(2)
    switch arguments[1] {
    case "create":
        let options = try parseOptions(rest, allowed: ["rp", "user"])
        try requireOptions(options, ["rp", "user"])
        let (rp, user) = try validated(options)
        try emit(try create(rpId: rp!, username: user!))
    case "assert":
        let options = try parseOptions(rest, allowed: ["rp", "user"])
        try requireOptions(options, ["rp", "user"])
        let (rp, user) = try validated(options)
        try emit(try assertWithSecureEnclave(rpId: rp!, username: user!))
    case "list":
        let options = try parseOptions(rest, allowed: ["rp", "user"])
        let (rp, user) = try validated(options)
        try emit(try list(rpId: rp, username: user))
    case "delete":
        let options = try parseOptions(rest, allowed: ["rp", "user", "credential-id"])
        try requireOptions(options, ["rp", "user"])
        let (rp, user) = try validated(options)
        try emit(try delete(rpId: rp!, username: user!, credentialId: options["credential-id"]))
    case "self-test":
        guard rest.isEmpty else { throw HelperError(.invalidInput, usage) }
        try emit(try selfTest())
    default:
        throw HelperError(.invalidInput, usage)
    }
    exit(ExitCode.ok.rawValue)
} catch let error as HelperError {
    FileHandle.standardError.write(Data("plank-passkey: \(error.message)\n".utf8))
    exit(error.code.rawValue)
} catch {
    FileHandle.standardError.write(Data("plank-passkey: \(error)\n".utf8))
    exit(ExitCode.failure.rawValue)
}
