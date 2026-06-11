#!/usr/bin/env py
"""Generate a minimal test.msi (OLE2 + MSZIP cab + registry) and test.msix
(ZIP/OPC) for exercising the OsitoK native installer engine."""
import os, sys, zipfile
import msilib
from msilib import schema, sequence

OUT = sys.argv[1] if len(sys.argv) > 1 else "."
os.makedirs(OUT, exist_ok=True)

# ── payload file on disk ────────────────────────────────────────
payload = os.path.join(OUT, "hello.txt")
content = b"Hello from the OsitoK MSI installer engine!\n" * 8  # >64B, compresses
with open(payload, "wb") as f:
    f.write(content)

# ── MSI ─────────────────────────────────────────────────────────
msi_path = os.path.join(OUT, "test.msi")
if os.path.exists(msi_path):
    os.remove(msi_path)

db = msilib.init_database(msi_path, schema, "OsitoTest",
                          "{0F3D2A10-1111-2222-3333-444455556666}",
                          "1.0.0", "NaranjositosTech")
msilib.add_tables(db, sequence)

msilib.add_data(db, "Directory", [
    ("TARGETDIR",          None,                 "SourceDir"),
    ("ProgramFilesFolder", "TARGETDIR",          "."),
    ("AppDir",             "ProgramFilesFolder", "OsitoApp"),
])

msilib.add_data(db, "Component", [
    ("MainComp", "{0F3D2A10-AAAA-BBBB-CCCC-444455556666}",
     "AppDir", 0, None, "hello.txt"),
])

msilib.add_data(db, "Feature", [
    ("MainFeat", None, "Main", "Main feature", 1, 1, None, 0),
])
msilib.add_data(db, "FeatureComponents", [("MainFeat", "MainComp")])

cab = msilib.CAB("testcab")
logical = "hello.txt"
seq = cab.append(payload, "hello.txt", logical)
# cab.append returns (index, logical) in CPython; normalize to an int sequence
seqno = seq[0] if isinstance(seq, tuple) else seq

msilib.add_data(db, "File", [
    (logical, "MainComp", "hello.txt", len(content), None, None, 0, seqno),
])
# cab.commit() inserts the matching Media row (DiskId, LastSequence, "#testcab")
cab.commit(db)

msilib.add_data(db, "Registry", [
    ("reg_ver", 2, r"Software\OsitoApp", "Version", "1.0.0", "MainComp"),
    ("reg_path", 2, r"Software\OsitoApp", "InstallDir",
     r"[ProgramFilesFolder]OsitoApp", "MainComp"),
])

db.Commit()
print("wrote", msi_path, os.path.getsize(msi_path), "bytes")

# ── MSIX (ZIP/OPC) ──────────────────────────────────────────────
msix_path = os.path.join(OUT, "test.msix")
manifest = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10">\n'
    '  <Identity Name="OsitoTestApp" Publisher="CN=Naranjositos" Version="2.3.4.0"/>\n'
    '  <Applications>\n'
    '    <Application Id="App" Executable="OsitoApp.exe" EntryPoint="App">\n'
    '    </Application>\n'
    '  </Applications>\n'
    '</Package>\n'
)
content_types = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n'
    '  <Default Extension="xml" ContentType="text/xml"/>\n'
    '  <Default Extension="txt" ContentType="text/plain"/>\n'
    '</Types>\n'
)
with zipfile.ZipFile(msix_path, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", content_types)
    z.writestr("AppxManifest.xml", manifest)
    z.writestr("OsitoApp.exe", b"MZ\x90\x00 fake payload exe for OsitoK MSIX test\n" * 4)
    z.writestr("assets/readme.txt", b"MSIX payload asset file.\n")
print("wrote", msix_path, os.path.getsize(msix_path), "bytes")
