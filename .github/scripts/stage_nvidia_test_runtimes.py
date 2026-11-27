# This file is part of Telegram Desktop,
# the official desktop application for the Telegram messaging service.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

import argparse
import hashlib
import struct
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

WHEEL_URL = (
    "https://pypi.nvidia.com/nvidia-vfx/"
    "nvidia_vfx-0.2.0.0-cp311-cp311-win_amd64.whl"
)
WHEEL_SHA256 = (
    "5aaf6a42bc6b6dbbf52fcb714194c994a6893cbbf7ada38bc2165a1f83e4a6fc"
)
WHEEL_VERSION = "0.2.0.0"
VFX_DLLS = (
    "NVVideoEffects.dll",
    "NVCVImage.dll",
    "nvVFXVideoFrameGeneration.dll",
    "nvngxruntime.dll",
    "cudart64_12.dll",
)
LICENSE_PREFIX = "nvidia_vfx-0.2.0.0.dist-info/licenses/packaging/"

NR_URL = (
    "https://github.com/Likely7/Veyra-NRVideo/releases/download/"
    "v2.2.1/Veyra-2.2.1-NVIDIA-win64-portable.zip"
)
NR_INNER = (
    "Veyra-2.2.1-NVIDIA-win64-portable/versions/2.2.1/"
    "runtime/experimental/nr-original/nvngx_dlssnr.dll"
)
NR_SHA256 = (
    "E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E"
)
NR_VERSION = "310.8.0.0"


def download(url, dest):
    request = urllib.request.Request(
        url,
        headers={"User-Agent": "fishgram-windows-test"})
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=300) as response:
        with dest.open("wb") as out:
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                digest.update(chunk)
                out.write(chunk)
    return digest.hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(1024 * 1024)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def file_version(path):
    import ctypes
    from ctypes import wintypes

    version = ctypes.windll.version
    dummy = wintypes.DWORD()
    size = version.GetFileVersionInfoSizeW(str(path), ctypes.byref(dummy))
    if not size:
        return "unknown"
    buffer = ctypes.create_string_buffer(size)
    if not version.GetFileVersionInfoW(str(path), 0, size, buffer):
        return "unknown"
    value = ctypes.c_void_p()
    length = wintypes.UINT()
    if not version.VerQueryValueW(
            buffer,
            "\\",
            ctypes.byref(value),
            ctypes.byref(length)):
        return "unknown"
    if length.value < 16:
        return "unknown"
    fixed = ctypes.string_at(value, length.value)
    high, low = struct.unpack_from("<II", fixed, 8)
    return (
        f"{(high >> 16) & 0xFFFF}.{high & 0xFFFF}."
        f"{(low >> 16) & 0xFFFF}.{low & 0xFFFF}"
    )


def require_hash(label, actual, expected):
    if actual.lower() != expected.lower():
        raise SystemExit(
            f"{label} SHA-256 is {actual}, expected {expected}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--stage", required=True)
    args = parser.parse_args()
    stage = Path(args.stage)
    if not stage.is_dir():
        raise SystemExit(f"Stage folder is missing: {stage}")

    vfx = stage / "vfx"
    licenses = vfx / "licenses"
    ngx = stage / "ngx"
    vfx.mkdir()
    licenses.mkdir()
    ngx.mkdir()

    rows = []
    with tempfile.TemporaryDirectory(prefix="nvidia-test-") as temp_name:
        temp = Path(temp_name)
        wheel = temp / "nvidia_vfx.whl"
        print(f"Downloading {WHEEL_URL}", flush=True)
        wheel_hash = download(WHEEL_URL, wheel)
        require_hash("nvidia-vfx wheel", wheel_hash, WHEEL_SHA256)
        with zipfile.ZipFile(wheel) as archive:
            names = set(archive.namelist())
            for dll in VFX_DLLS:
                member = f"nvvfx/libs/{dll}"
                if member not in names:
                    raise SystemExit(f"Wheel is missing {member}")
                target = vfx / dll
                target.write_bytes(archive.read(member))
                rows.append((
                    f"vfx\\{dll}",
                    WHEEL_URL,
                    file_version(target),
                    sha256_file(target),
                    f"package nvidia-vfx {WHEEL_VERSION}",
                ))
            license_names = sorted(
                name for name in names
                if name.startswith(LICENSE_PREFIX) and not name.endswith("/"))
            if not license_names:
                raise SystemExit(
                    f"Wheel is missing {LICENSE_PREFIX}")
            for name in license_names:
                target = licenses / Path(name).name
                target.write_bytes(archive.read(name))
                rows.append((
                    f"vfx\\licenses\\{target.name}",
                    WHEEL_URL,
                    WHEEL_VERSION,
                    sha256_file(target),
                    "license from the nvidia-vfx wheel",
                ))

        bundle = temp / "veyra-nvidia.zip"
        print(f"Downloading {NR_URL}", flush=True)
        download(NR_URL, bundle)
        with zipfile.ZipFile(bundle) as archive:
            try:
                payload = archive.read(NR_INNER)
            except KeyError:
                raise SystemExit(
                    f"Veyra package is missing {NR_INNER}") from None
        nr_hash = hashlib.sha256(payload).hexdigest()
        require_hash("nvngx_dlssnr.dll", nr_hash, NR_SHA256)
        nr_path = ngx / "nvngx_dlssnr.dll"
        nr_path.write_bytes(payload)
        copied = sha256_file(nr_path)
        require_hash("staged nvngx_dlssnr.dll", copied, NR_SHA256)
        rows.append((
            "ngx\\nvngx_dlssnr.dll",
            NR_URL,
            file_version(nr_path),
            copied,
            f"archive path {NR_INNER}; pinned file version {NR_VERSION}",
        ))

    notice = stage / "THIRD_PARTY_NVIDIA.txt"
    lines = [
        "NVIDIA files staged only in this Windows test package.",
        "They are not part of the git tree and are not a release.",
        "No FISHGRAM_VFX_RUNTIME or FISHGRAM_NGX_RUNTIME variable is set.",
        "Frame generation loads vfx next to Telegram.exe.",
        "DLSS NR loads ngx next to Telegram.exe.",
        "",
    ]
    for relative, url, version, digest, note in rows:
        lines.extend([
            relative,
            f"source: {url}",
            f"version: {version}",
            f"sha256: {digest}",
            note,
            "",
        ])
    notice.write_text("\r\n".join(lines) + "\r\n", encoding="utf-8")
    print(f"Wrote {notice}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
