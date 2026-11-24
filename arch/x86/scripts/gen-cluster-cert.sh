#!/bin/bash
# gen-cluster-cert.sh — generate a self-signed P-256 cert + raw
# 32-byte private key for the osito-a TLS 1.3 server (cluster mode)
# and bake them into the nvme.img at osfs2 paths:
#   cluster/tls-cert.der   - DER-encoded leaf certificate
#   cluster/tls-key.bin    - 32-byte big-endian P-256 scalar
#
# Usage:  bash arch/x86/scripts/gen-cluster-cert.sh [nvme.img]
#         (defaults to arch/x86/build/nvme.img)

set -e
set -o pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
X86_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$X86_DIR/../.." && pwd)"
BUILD="$X86_DIR/build"
NVME="${1:-$BUILD/nvme.img}"
OFS_TOOLS="$REPO_DIR/tools/ositofs"
[ -f "$NVME" ] || { echo "no nvme image at $NVME"; exit 1; }
[ -x "$OFS_TOOLS/ositofs-write" ] || { echo "ositofs-write missing"; exit 1; }

WORK=$(mktemp -d)
trap "rm -rf $WORK" EXIT

# 1. Private key (P-256, PEM)
openssl ecparam -name prime256v1 -genkey -noout -out "$WORK/key.pem"

# 2. Self-signed cert, 5y, CN=osito-a-node
openssl req -x509 -new -key "$WORK/key.pem" \
    -days 1825 \
    -subj "/CN=osito-a-node" \
    -out "$WORK/cert.pem" -outform PEM \
    -sha256 \
    -addext "subjectAltName=DNS:osito-a-node,DNS:localhost" \
    -addext "basicConstraints=critical,CA:false" \
    -addext "keyUsage=critical,digitalSignature,keyAgreement"

# 3. Convert cert PEM → DER
openssl x509 -in "$WORK/cert.pem" -outform DER -out "$WORK/cert.der"

# 4. Extract the raw 32-byte private scalar from the EC private key
#    asn1parse reveals the structure:
#      SEQUENCE
#        INTEGER 1
#        OCTET STRING <32-byte d>
#        ...
#    The OCTET STRING is what we want — grab its hex via -noout -strparse.
openssl asn1parse -in "$WORK/key.pem" -out "$WORK/asn1.bin" >/dev/null
# The OCTET STRING contents start at offset 7 (TAG 04 LEN 20)
# in the inner SEQUENCE; simpler: regenerate explicit point + dump
openssl ec -in "$WORK/key.pem" -text -noout 2>/dev/null \
    | awk '/priv:/{p=1;next} p && /^[^ ]/{p=0} p {print}' \
    | tr -d ' :\n' \
    | sed 's/^00//' \
    | xxd -r -p > "$WORK/key.bin"

KEY_LEN=$(wc -c < "$WORK/key.bin" | tr -d ' ')
if [ "$KEY_LEN" != 32 ]; then
    echo "ERR: extracted private key is $KEY_LEN B, expected 32" >&2
    exit 1
fi

# 5. Bake into the nvme image at osfs2 paths cluster/tls-cert.der
#    and cluster/tls-key.bin.  We use the existing ositofs-write tool
#    which writes from a host file with the same basename — so we
#    rename our outputs to match the on-disk paths first.
mkdir -p "$WORK/cluster"
cp "$WORK/cert.der" "$WORK/cluster/tls-cert.der"
cp "$WORK/key.bin"  "$WORK/cluster/tls-key.bin"
"$OFS_TOOLS/ositofs-write"  "$NVME" "$WORK/cluster/tls-cert.der" \
    --name "cluster/tls-cert.der" --overwrite | tail -1
"$OFS_TOOLS/ositofs-write"  "$NVME" "$WORK/cluster/tls-key.bin" \
    --name "cluster/tls-key.bin"  --overwrite | tail -1

echo "[gen-cluster-cert] wrote cluster/tls-cert.der ($(wc -c < "$WORK/cluster/tls-cert.der" | tr -d ' ') B)"
echo "[gen-cluster-cert] wrote cluster/tls-key.bin (32 B)"
