"""apk_add.py <base.apk> <out.apk> <classes.dex> <lib dir>
Copy the aapt2 output and add classes.dex and lib/<abi>/*.so (compressed; the manifest sets
extractNativeLibs). Entries aapt2 stored uncompressed (the voice model) stay stored."""
import os
import sys
import zipfile

base, out, dex, libdir = sys.argv[1:5]
with zipfile.ZipFile(base) as zin, zipfile.ZipFile(out, "w") as zout:
    for info in zin.infolist():
        zout.writestr(info, zin.read(info.filename), compress_type=info.compress_type)
    zout.write(dex, "classes.dex", compress_type=zipfile.ZIP_DEFLATED)
    for root, _, files in os.walk(libdir):
        for f in files:
            path = os.path.join(root, f)
            arc = "lib/" + os.path.relpath(path, libdir).replace(os.sep, "/")
            zout.write(path, arc, compress_type=zipfile.ZIP_DEFLATED)
print("apk:", out)
