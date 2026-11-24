#!/usr/bin/env python3
"""Turn a PEM bundle of root CAs into BearSSL trust anchors (C source).

The kernels validate server certificates with BearSSL's X.509 engine, which
takes its roots as br_x509_trust_anchor entries: the subject's encoded DN and
the public key, RSA (modulus, exponent) or EC (curve, point). This is what
BearSSL's `brssl ta` prints, without needing brssl.

Usage:
    gen-bearssl-anchors.py <bundle.pem> <out.c> [--include <bearssl.h path>]

The bundle is curl's extract of Mozilla's roots: https://curl.se/ca/cacert.pem
(its SHA-256 is published next to it, cacert.pem.sha256). The generated file
records the bundle's date and hash. Roots with keys BearSSL can't use are
skipped and listed on stderr.
"""
import base64
import hashlib
import re
import sys

OID_RSA = bytes.fromhex("2a864886f70d010101")       # rsaEncryption
OID_EC = bytes.fromhex("2a8648ce3d0201")            # id-ecPublicKey
CURVES = {                                          # BearSSL curve ids
    bytes.fromhex("2a8648ce3d030107"): ("BR_EC_secp256r1", 23),
    bytes.fromhex("2b81040022"): ("BR_EC_secp384r1", 24),
    bytes.fromhex("2b81040023"): ("BR_EC_secp521r1", 25),
}
OID_CN = bytes.fromhex("550403")


def tlv(buf, pos):
    """(tag, header length, value length) of the DER element at pos"""
    tag = buf[pos]
    n = buf[pos + 1]
    if n < 0x80:
        return tag, 2, n
    k = n & 0x7F
    return tag, 2 + k, int.from_bytes(buf[pos + 2:pos + 2 + k], "big")


def children(buf, pos):
    """The elements inside the constructed element at pos"""
    _, hl, vl = tlv(buf, pos)
    p, end = pos + hl, pos + hl + vl
    out = []
    while p < end:
        _, h, v = tlv(buf, p)
        out.append(p)
        p += h + v
    return out


def element(buf, pos):
    _, hl, vl = tlv(buf, pos)
    return buf[pos:pos + hl + vl]


def value(buf, pos):
    _, hl, vl = tlv(buf, pos)
    return buf[pos + hl:pos + hl + vl]


def common_name(der, name_pos):
    for rdn in children(der, name_pos):
        for atv in children(der, rdn):
            oid, val = children(der, atv)
            if value(der, oid) == OID_CN:
                return value(der, val).decode("utf-8", "replace")
    return "?"


def anchor(der):
    """(DN, key kind, key fields, CN) of a certificate"""
    tbs = children(der, 0)[0]
    fields = children(der, tbs)
    if der[fields[0]] == 0xA0:                      # [0] version
        fields = fields[1:]
    subject, spki = fields[4], fields[5]
    alg, bits = children(der, spki)
    alg_fields = children(der, alg)
    key = value(der, bits)[1:]                      # the unused-bits byte off
    oid = value(der, alg_fields[0])
    cn = common_name(der, subject)
    if oid == OID_RSA:
        n_pos, e_pos = children(key, 0)
        n = value(key, n_pos).lstrip(b"\0")
        e = value(key, e_pos).lstrip(b"\0")
        return element(der, subject), "rsa", (n, e), cn
    if oid == OID_EC and len(alg_fields) > 1:
        curve = CURVES.get(value(der, alg_fields[1]))
        if curve:
            return element(der, subject), "ec", (curve[0], key), cn
    return None, None, None, cn


def c_bytes(name, data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12]) + ",")
    return "static const unsigned char %s[] = {\n%s\n};\n" % (name, "\n".join(lines))


def main():
    args = sys.argv[1:]
    include = "bearssl.h"
    if "--include" in args:
        i = args.index("--include")
        include = args[i + 1]
        del args[i:i + 2]
    if len(args) != 2:
        sys.exit(__doc__)
    pem_path, out_path = args
    pem = open(pem_path, "rb").read()
    date = re.search(rb"Certificate data from Mozilla as of: ([^\n]+)", pem)
    certs = re.findall(rb"-----BEGIN CERTIFICATE-----(.+?)-----END CERTIFICATE-----",
                       pem, re.S)
    body, table, skipped = [], [], []
    for der in (base64.b64decode(b"".join(c.split())) for c in certs):
        dn, kind, key, cn = anchor(der)
        if not dn:
            skipped.append(cn)
            continue
        i = len(table)
        ascii_cn = cn.encode("ascii", "backslashreplace").decode("ascii")
        body.append("/* %s */\n" % ascii_cn.replace("*/", "* /"))
        body.append(c_bytes("TA%d_DN" % i, dn))
        if kind == "rsa":
            body.append(c_bytes("TA%d_RSA_N" % i, key[0]))
            body.append(c_bytes("TA%d_RSA_E" % i, key[1]))
            table.append("    { { (unsigned char *)TA%d_DN, sizeof TA%d_DN }, BR_X509_TA_CA,\n"
                         "      { BR_KEYTYPE_RSA, { .rsa = { (unsigned char *)TA%d_RSA_N, sizeof TA%d_RSA_N,\n"
                         "                                   (unsigned char *)TA%d_RSA_E, sizeof TA%d_RSA_E } } } },"
                         % (i, i, i, i, i, i))
        else:
            body.append(c_bytes("TA%d_EC_Q" % i, key[1]))
            table.append("    { { (unsigned char *)TA%d_DN, sizeof TA%d_DN }, BR_X509_TA_CA,\n"
                         "      { BR_KEYTYPE_EC, { .ec = { %s, (unsigned char *)TA%d_EC_Q, sizeof TA%d_EC_Q } } } },"
                         % (i, i, key[0], i, i))
        body.append("\n")
    with open(out_path, "w", encoding="ascii", newline="\n") as out:
        out.write("/*\n * Root CAs as BearSSL trust anchors: %d of Mozilla's, from curl's\n"
                  " * cacert.pem (Mozilla data as of %s, SHA-256 %s).\n"
                  " * Generated by tools/ca-roots/gen-bearssl-anchors.py: don't edit.\n */\n\n"
                  % (len(table), date.group(1).decode() if date else "unknown",
                     hashlib.sha256(pem).hexdigest()))
        out.write('#include "%s"\n\n' % include)
        out.write("".join(body))
        out.write("const br_x509_trust_anchor ca_roots[] = {\n%s\n};\n\n" % "\n".join(table))
        out.write("const size_t ca_roots_count = sizeof ca_roots / sizeof ca_roots[0];\n")
    print("%d anchors -> %s" % (len(table), out_path))
    for cn in skipped:
        print("skipped (unsupported key): %s" % cn, file=sys.stderr)


if __name__ == "__main__":
    main()
