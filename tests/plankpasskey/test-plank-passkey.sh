#!/usr/bin/env bash
# plank-passkey helper checks that need no Touch ID (bde-linux
# docs/plank-broker.md 13.1/13.4). The signature produced by the helper's
# shared assertion code (with a software key, `self-test`) is verified
# independently with OpenSSL: authData is rebuilt here from the rp id, and
# ECDSA-P256-SHA256(authData || challenge) is checked against the SPKI.
#
# usage: test-plank-passkey.sh HELPER [--secure-enclave]
#   --secure-enclave also runs create/list/delete against the real Secure
#   Enclave in a temporary store (key creation needs no user presence).
set -euo pipefail
helper=${1:?usage: test-plank-passkey.sh HELPER [--secure-enclave]}
[[ $helper == /* ]] || helper=$PWD/$helper
openssl=${PLANK_TEST_OPENSSL:-openssl}
# LibreSSL's `openssl` lacks some pkey options; prefer a real OpenSSL 3.
if ! "$openssl" version | grep -q '^OpenSSL 3'; then
    for candidate in /opt/homebrew/bin/openssl /usr/local/bin/openssl; do
        [[ -x $candidate ]] && "$candidate" version | grep -q '^OpenSSL 3' && openssl=$candidate && break
    done
fi
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }
field() { python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"; }

rp=ipa.bde.run
challenge_raw="$work/challenge.bin"
head -c 32 /dev/urandom > "$challenge_raw"
challenge=$(base64 < "$challenge_raw" | tr -d '\n')
credid=$(head -c 32 /dev/urandom | base64 | tr -d '\n')
other=$(head -c 32 /dev/urandom | base64 | tr -d '\n')
prompt=$(printf '{"rp_id":"%s","credential_ids":["%s","%s"],"user_verification":true,"challenge":"%s"}' \
    "$rp" "$credid" "$other" "$challenge")

# --- format proof: software key through the shared assertion code -------------
for round in 1 2 3; do
    out=$(printf '%s' "$prompt" | "$helper" self-test)
    [[ $(field credential_id <<<"$out") == "$credid" ]] || fail "credential id not echoed"
    field public_key <<<"$out" | base64 -D > "$work/spki.der"
    field authenticator_data <<<"$out" | base64 -D > "$work/authdata.bin"
    field signature <<<"$out" | base64 -D > "$work/sig.der"
    [[ $(field mapping <<<"$out") == "passkey:$credid,$(field public_key <<<"$out")" ]] || fail "mapping format"

    # authData = SHA-256(rp_id) || 0x05 || 00 00 00 00, rebuilt independently.
    { printf '%s' "$rp" | "$openssl" dgst -sha256 -binary; printf '\005\000\000\000\000'; } > "$work/expected-authdata.bin"
    cmp -s "$work/authdata.bin" "$work/expected-authdata.bin" || fail "authenticator_data layout"
    [[ $(wc -c < "$work/authdata.bin") -eq 37 ]] || fail "authenticator_data length"

    # SPKI must be an uncompressed P-256 public key.
    "$openssl" pkey -pubin -inform DER -in "$work/spki.der" -out "$work/pub.pem"
    "$openssl" pkey -pubin -in "$work/pub.pem" -text -noout | grep -q 'prime256v1\|P-256' || fail "not P-256"

    cat "$work/authdata.bin" "$challenge_raw" > "$work/message.bin"
    "$openssl" dgst -sha256 -verify "$work/pub.pem" -signature "$work/sig.der" "$work/message.bin" >/dev/null ||
        fail "signature does not verify as ECDSA-P256-SHA256(authData || challenge)"
    # A flipped challenge bit or message without the challenge must not verify.
    cp "$work/message.bin" "$work/tampered.bin"
    printf '\377' | dd of="$work/tampered.bin" bs=1 seek=40 conv=notrunc 2>/dev/null
    if "$openssl" dgst -sha256 -verify "$work/pub.pem" -signature "$work/sig.der" "$work/tampered.bin" >/dev/null 2>&1; then
        fail "tampered message verified"
    fi
    if "$openssl" dgst -sha256 -verify "$work/pub.pem" -signature "$work/sig.der" "$work/authdata.bin" >/dev/null 2>&1; then
        fail "authData alone verified"
    fi
done
echo "self_test_signature=verified-openssl"

# --- input validation ----------------------------------------------------------
expect_exit() {
    local expected=$1 input=$2; shift 2
    set +e
    printf '%s' "$input" | "$@" >/dev/null 2>&1
    local status=$?
    set -e
    [[ $status -eq $expected ]] || fail "expected exit $expected, got $status: $* <<< $input"
}
short=$(head -c 31 /dev/urandom | base64 | tr -d '\n')
long=$(head -c 33 /dev/urandom | base64 | tr -d '\n')
unpadded=${challenge%=}
expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":["%s"],"challenge":"%s"}' "$rp" "$credid" "$short")" "$helper" self-test
expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":["%s"],"challenge":"%s"}' "$rp" "$credid" "$long")" "$helper" self-test
expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":["%s"],"challenge":"%s"}' "$rp" "$credid" "$unpadded")" "$helper" self-test
for bad_rp in 'IPA.BDE.RUN' 'https://ipa.bde.run' 'ipa.bde.run.' '192.168.10.240' 'ipa..run' '-ipa.run' 'ipa.bde.run:443' ''; do
    expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":["%s"],"challenge":"%s"}' "$bad_rp" "$credid" "$challenge")" "$helper" self-test
done
expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":[],"challenge":"%s"}' "$rp" "$challenge")" "$helper" self-test
expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":["not base64!"],"challenge":"%s"}' "$rp" "$challenge")" "$helper" self-test
expect_exit 2 "$(printf '{"rp_id":"%s","credential_ids":["%s"],"challenge":"%s","user_verification":"yes"}' "$rp" "$credid" "$challenge")" "$helper" self-test
expect_exit 2 'not json' "$helper" self-test
expect_exit 2 '' "$helper" create --rp "$rp" --user ../evil
expect_exit 2 '' "$helper" create --rp "$rp" --user Anna
expect_exit 2 '' "$helper" create --rp 'ipa.bde.run/x' --user anna
expect_exit 2 '' "$helper" list --bogus x
expect_exit 2 '' "$helper"
echo "input_validation=pass"

# --- store: no key -> exit 3 (the Client falls back to password + code) ---------
export PLANK_PASSKEY_STORE="$work/store"
[[ $("$helper" list) == '[]' ]] || fail "empty store must list []"
expect_exit 3 "$prompt" "$helper" assert --rp "$rp" --user anna
expect_exit 3 '' "$helper" delete --rp "$rp" --user anna
echo "no_key_fallback_exit=3"

if [[ ${2:-} == --secure-enclave ]]; then
    created=$("$helper" create --rp "$rp" --user plank-selftest)
    id=$(field credential_id <<<"$created")
    [[ $(field mapping <<<"$created") == "passkey:$id,$(field public_key <<<"$created")" ]] || fail "create mapping"
    field public_key <<<"$created" | base64 -D | "$openssl" pkey -pubin -inform DER -noout || fail "create SPKI"
    [[ $(printf '%s' "$id" | base64 -D | wc -c) -eq 32 ]] || fail "credential id length"
    file=$(find "$PLANK_PASSKEY_STORE" -name '*.json')
    [[ $(stat -f %Lp "$file") == 600 ]] || fail "key file mode"
    for dir in "$PLANK_PASSKEY_STORE" "$PLANK_PASSKEY_STORE/$rp" "$PLANK_PASSKEY_STORE/$rp/plank-selftest"; do
        [[ $(stat -f %Lp "$dir") == 700 ]] || fail "directory mode $dir"
    done
    listed=$("$helper" list --rp "$rp" --user plank-selftest)
    python3 -c 'import json,sys; l=json.loads(sys.argv[1]); assert len(l)==1 and l[0]["credential_id"]==sys.argv[2] and "key" not in l[0]' \
        "$listed" "$id" || fail "list"
    # A prompt that does not allow this credential never reaches Touch ID.
    expect_exit 3 "$prompt" "$helper" assert --rp "$rp" --user plank-selftest
    [[ $("$helper" delete --rp "$rp" --user plank-selftest) == '{"deleted":1}' ]] || fail "delete"
    [[ $("$helper" list) == '[]' ]] || fail "list after delete"
    echo "secure_enclave_create_list_delete=pass"
fi
echo "plank_passkey_helper_tests=pass"
