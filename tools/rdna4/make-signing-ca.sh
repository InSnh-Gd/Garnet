#!/bin/bash
# Generate a local offline RSA 4096/2048 signing chain compatible with AMFIAnchor
# (Code Signing EKU + Apple platform marker OID 1.2.840.113635.100.6.22).
# Usage: bash tools/rdna4/make-signing-ca.sh <OUT_DIR> [CN_PREFIX]
# OUT_DIR must live OUTSIDE the Git repository so private keys are never committed.
set -euo pipefail
umask 077

fail() { printf 'make-signing-ca.sh: %s\n' "$*" >&2; exit 1; }
[ "$#" -ge 1 ] && [ "$#" -le 2 ] && [ -n "$1" ] || fail 'Usage: make-signing-ca.sh <OUT_DIR> [CN_PREFIX]'

repo=$(cd "$(dirname "$0")/../.." && pwd -P)
out_raw=$1
cn_prefix=${2:-Garnet Local}
OPENSSL_BIN=${GARNET_OPENSSL_BIN:-openssl}
command -v "$OPENSSL_BIN" >/dev/null 2>&1 || fail "Required tool not found: $OPENSSL_BIN"

case "$out_raw" in /*) ;; *) out_raw="$PWD/$out_raw";; esac
[ ! -e "$out_raw" ] && [ ! -L "$out_raw" ] || fail "OUT_DIR already exists (refusing to overwrite existing keys): $out_raw"
parent=$(dirname "$out_raw")
mkdir -p "$parent"
parent_real=$(cd "$parent" && pwd -P)
out="$parent_real/$(basename "$out_raw")"
case "$out/" in
    "$repo/"*) fail "Refusing to store CA private keys inside repository: $out";;
esac

mkdir -p "$out"
profile="$out/signing-ca.cnf"
cat > "$profile" <<'EOF'
[req]
distinguished_name = req_dn
x509_extensions = root_ca
prompt = no

[req_dn]
CN = Garnet Signing CA

[root_ca]
basicConstraints = critical, CA:true, pathlen:1
keyUsage = critical, keyCertSign, cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always

[intermediate_ca]
basicConstraints = critical, CA:true, pathlen:0
keyUsage = critical, keyCertSign, cRLSign
extendedKeyUsage = codeSigning
1.2.840.113635.100.6.22 = ASN1:NULL
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always

[code_signing]
basicConstraints = critical, CA:false
keyUsage = critical, digitalSignature
extendedKeyUsage = codeSigning
1.2.840.113635.100.6.22 = ASN1:NULL
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
EOF

"$OPENSSL_BIN" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out "$out/root.key.pem"
"$OPENSSL_BIN" req -new -x509 -sha256 -days 3650 -key "$out/root.key.pem" \
    -subj "/CN=${cn_prefix} Root CA" -config "$profile" -extensions root_ca -out "$out/root.cert.pem"

"$OPENSSL_BIN" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:4096 -out "$out/intermediate.key.pem"
"$OPENSSL_BIN" req -new -sha256 -key "$out/intermediate.key.pem" \
    -subj "/CN=${cn_prefix} Code Signing CA" -out "$out/intermediate.csr.pem"
"$OPENSSL_BIN" x509 -req -sha256 -days 1825 -in "$out/intermediate.csr.pem" \
    -CA "$out/root.cert.pem" -CAkey "$out/root.key.pem" -CAcreateserial \
    -extfile "$profile" -extensions intermediate_ca -out "$out/intermediate.cert.pem"

"$OPENSSL_BIN" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$out/leaf.key.pem"
"$OPENSSL_BIN" req -new -sha256 -key "$out/leaf.key.pem" \
    -subj "/CN=${cn_prefix} Software Signing" -out "$out/leaf.csr.pem"
"$OPENSSL_BIN" x509 -req -sha256 -days 365 -in "$out/leaf.csr.pem" \
    -CA "$out/intermediate.cert.pem" -CAkey "$out/intermediate.key.pem" -CAcreateserial \
    -extfile "$profile" -extensions code_signing -out "$out/leaf.cert.pem"

rm -f "$out/intermediate.csr.pem" "$out/leaf.csr.pem"
chmod 600 "$out"/*.key.pem
chmod 644 "$out"/*.cert.pem

"$OPENSSL_BIN" verify -CAfile "$out/root.cert.pem" -untrusted "$out/intermediate.cert.pem" "$out/leaf.cert.pem"

printf 'Created local AMFIAnchor signing chain in: %s\n' "$out"
printf 'Keep *.key.pem private and outside Git. Build with:\n'
printf '  GARNET_SIGN_CA_DIR=%s bash tools/rdna4/build-all.sh <OUT_DIR>\n' "$out"
