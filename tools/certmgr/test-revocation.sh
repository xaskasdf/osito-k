#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work_dir="${REVOCATION_FIXTURE_DIR:-$repo_root/arch/x86/build/revocation-fixtures}"
rm -rf "$work_dir"
mkdir -p "$work_dir/newcerts"
touch "$work_dir/index.txt"
printf '1000\n' > "$work_dir/serial"
printf '1000\n' > "$work_dir/crlnumber"

export REVOCATION_FIXTURE_DIR="$work_dir"
cat > "$work_dir/openssl.cnf" <<'EOF'
[ ca ]
default_ca = local_ca

[ local_ca ]
dir = $ENV::REVOCATION_FIXTURE_DIR
database = $dir/index.txt
new_certs_dir = $dir/newcerts
certificate = $dir/issuer.pem
private_key = $dir/issuer.key
serial = $dir/serial
crlnumber = $dir/crlnumber
default_md = sha256
default_days = 30
default_crl_days = 7
policy = policy_any
unique_subject = no
copy_extensions = copy

[ policy_any ]
commonName = supplied

[ req ]
distinguished_name = dn
prompt = no

[ dn ]
CN = unused

[ issuer_ext ]
basicConstraints = critical,CA:true,pathlen:1
keyUsage = critical,keyCertSign,cRLSign,digitalSignature
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always

[ leaf_ext ]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
authorityInfoAccess = OCSP;URI:http://ocsp.invalid/status
crlDistributionPoints = URI:http://crl.invalid/test.crl

[ responder_ext ]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,OCSPSigning
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid,issuer
EOF

openssl req -new -newkey rsa:2048 -nodes -subj '/CN=Osito Revocation Test CA' \
    -keyout "$work_dir/issuer.key" -out "$work_dir/issuer.csr" >/dev/null 2>&1
openssl x509 -req -sha256 -days 30 -in "$work_dir/issuer.csr" \
    -signkey "$work_dir/issuer.key" -extfile "$work_dir/openssl.cnf" \
    -extensions issuer_ext -out "$work_dir/issuer.pem" >/dev/null 2>&1

issue_certificate() {
    local name="$1"
    local extension="$2"
    openssl req -new -newkey rsa:2048 -nodes -subj "/CN=$name" \
        -keyout "$work_dir/$name.key" -out "$work_dir/$name.csr" >/dev/null 2>&1
    openssl ca -batch -config "$work_dir/openssl.cnf" -extensions "$extension" \
        -in "$work_dir/$name.csr" -out "$work_dir/$name.pem" >/dev/null 2>&1
}

issue_certificate leaf leaf_ext
issue_certificate other leaf_ext
issue_certificate responder responder_ext

openssl ocsp -index "$work_dir/index.txt" -CA "$work_dir/issuer.pem" \
    -rsigner "$work_dir/responder.pem" -rkey "$work_dir/responder.key" \
    -issuer "$work_dir/issuer.pem" -cert "$work_dir/leaf.pem" \
    -respout "$work_dir/good.ocsp.der" -ndays 1 >/dev/null 2>&1
openssl ca -config "$work_dir/openssl.cnf" -gencrl \
    -out "$work_dir/good.crl.pem" >/dev/null 2>&1

openssl ca -batch -config "$work_dir/openssl.cnf" \
    -revoke "$work_dir/leaf.pem" >/dev/null 2>&1
openssl ocsp -index "$work_dir/index.txt" -CA "$work_dir/issuer.pem" \
    -rsigner "$work_dir/responder.pem" -rkey "$work_dir/responder.key" \
    -issuer "$work_dir/issuer.pem" -cert "$work_dir/leaf.pem" \
    -respout "$work_dir/revoked.ocsp.der" -ndays 1 >/dev/null 2>&1
openssl ca -config "$work_dir/openssl.cnf" -gencrl \
    -out "$work_dir/revoked.crl.pem" >/dev/null 2>&1

for name in issuer leaf other; do
    openssl x509 -in "$work_dir/$name.pem" -outform DER \
        -out "$work_dir/$name.der"
done
for state in good revoked; do
    openssl crl -in "$work_dir/$state.crl.pem" -outform DER \
        -out "$work_dir/$state.crl.der"
done

cc -std=c11 -O2 -fno-builtin -ffunction-sections -fdata-sections \
    -I"$repo_root/arch/x86/include" \
    -I"$repo_root/arch/x86/kernel" \
    -I"$repo_root/arch/x86/fs" \
    "$repo_root/tools/certmgr/revocation-fixture-test.c" \
    "$repo_root/arch/x86/kernel/ocsp.c" \
    "$repo_root/arch/x86/kernel/crl.c" \
    "$repo_root/arch/x86/kernel/x509.c" \
    "$repo_root/arch/x86/kernel/ecdsa_p256.c" \
    "$repo_root/arch/x86/kernel/ecdsa_p384.c" \
    "$repo_root/arch/x86/kernel/rsa.c" \
    "$repo_root/arch/x86/kernel/crypto.c" \
    "$repo_root/arch/x86/kernel/crypto2.c" \
    -Wl,--gc-sections -o "$work_dir/revocation-fixture-test"

"$work_dir/revocation-fixture-test" \
    "$work_dir/issuer.der" "$work_dir/leaf.der" "$work_dir/other.der" \
    "$work_dir/good.ocsp.der" "$work_dir/revoked.ocsp.der" \
    "$work_dir/good.crl.der" "$work_dir/revoked.crl.der"
