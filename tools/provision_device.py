#!/usr/bin/env python3
"""
Pothole LTE — Device Provisioning Tool v1.0.0
==============================================
Writes TLS credentials AND device identity into the NVS partition of the
7Semi ESP32-S3 + EC200U-CN board so the firmware can load them at runtime.

Usage
-----
  # Full provisioning (certs + identity) — recommended first boot:
  python tools/provision_device.py \
      --port        COM20 \
      --cert        path/to/device.pem.crt \
      --key         path/to/private.pem.key \
      --device-id   pothole-unit-01 \
      --org-id      ORG-001 \
      --apn         airtelgprs.com \
      --endpoint    <your-iot-endpoint>.iot.ap-south-1.amazonaws.com

  # Cert-only update (identity already in NVS):
  python tools/provision_device.py \
      --port COM20 --cert device.pem.crt --key private.pem.key

  # Identity-only update (certs already in NVS):
  python tools/provision_device.py \
      --port COM20 --identity \
      --device-id pothole-unit-02 --org-id ORG-001 \
      --apn airtelgprs.com --endpoint <endpoint>

  Optional flags:
      [--baud 921600] [--nvs-address 0x9000] [--nvs-size 0x6000]
      [--idf-path C:\\esp\\esp-idf] [--dry-run]

Requirements
------------
  pip install esptool

  nvs_partition_gen.py is sourced from $IDF_PATH (or --idf-path).

NVS layout written
------------------
  Namespace : fleet_certs        ← SAME namespace as fleet_hub (board reuse)
  Key       : client_cert (blob) — device certificate PEM, null-terminated
  Key       : client_key  (blob) — device private key PEM, null-terminated

  Namespace : fleet_id
  Key       : device_id   (string) — e.g. "pothole-unit-01"
  Key       : org_id      (string) — e.g. "ORG-001"
  Key       : apn         (string) — cellular APN, e.g. "airtelgprs.com"
  Key       : iot_endpoint(string) — AWS IoT Core endpoint FQDN

  Note: "vehicle_id" is NOT used by pothole firmware (fleet_hub only).

Security notes
--------------
  - The private key is written to a secure temp dir during NVS generation
    and zero-wiped before the script exits.
  - The source key file is NOT deleted by this script.
  - Never commit private keys to source control.
"""

import argparse
import os
import subprocess
import sys
import tempfile

# ── NVS namespace / key constants (must match pothole_certs.h / pothole_config.h)
NVS_NS_CERTS   = "fleet_certs"
NVS_KEY_CERT   = "client_cert"
NVS_KEY_KEY    = "client_key"
CERT_MAX_BYTES = 2048
KEY_MAX_BYTES  = 2048

NVS_NS_IDENTITY    = "fleet_id"
NVS_KEY_DEVICE_ID  = "device_id"
NVS_KEY_ORG_ID     = "org_id"
NVS_KEY_APN        = "apn"
NVS_KEY_ENDPOINT   = "iot_endpoint"
IDENTITY_MAX_LEN   = 128

# ── Helpers ────────────────────────────────────────────────────────────────────

def die(msg: str) -> None:
    print(f"\n[ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


def secure_delete(path: str) -> None:
    """Overwrite file with zeros before unlinking."""
    try:
        size = os.path.getsize(path)
        with open(path, "wb") as f:
            f.write(b"\x00" * size)
        os.unlink(path)
    except Exception:
        pass  # best effort


def validate_pem(data: bytes, expected_header: str, label: str) -> None:
    text = data.decode("ascii", errors="replace")
    if expected_header not in text:
        die(
            f"{label}: does not look like a valid PEM file.\n"
            f"Expected header: '{expected_header}'"
        )


def validate_size(data: bytes, max_bytes: int, label: str) -> None:
    if len(data) + 1 > max_bytes:
        die(
            f"{label} is {len(data)} bytes — exceeds firmware buffer of "
            f"{max_bytes} bytes.  Increase POTHOLE_{label.upper()}_MAX_LEN "
            f"in pothole_certs.h."
        )


def find_nvs_partition_gen(idf_path_override) -> str:
    candidates = []
    if idf_path_override:
        candidates.append(
            os.path.join(idf_path_override,
                         "components", "nvs_flash",
                         "nvs_partition_generator", "nvs_partition_gen.py"))
    idf_env = os.environ.get("IDF_PATH")
    if idf_env:
        candidates.append(
            os.path.join(idf_env,
                         "components", "nvs_flash",
                         "nvs_partition_generator", "nvs_partition_gen.py"))
    for path in candidates:
        if os.path.isfile(path):
            return path
    die(
        "Cannot find nvs_partition_gen.py.\n"
        "Options:\n"
        "  1. Pass --idf-path, e.g.: --idf-path C:\\esp\\esp-idf\n"
        "  2. Set IDF_PATH environment variable.\n"
        "  3. Run inside the ESP-IDF environment (run export.bat / get_idf first)."
    )


def generate_nvs_binary(cert_data, key_data, identity,
                         output_bin, nvs_size, nvs_gen_script, tmpdir):
    csv_tmp = os.path.join(tmpdir, "pothole_nvs.csv")
    with open(csv_tmp, "w", newline="\n") as f:
        f.write("key,type,encoding,value\n")

        if cert_data is not None and key_data is not None:
            cert_tmp = os.path.join(tmpdir, "client_cert.bin")
            key_tmp  = os.path.join(tmpdir, "client_key.bin")
            with open(cert_tmp, "wb") as cf:
                cf.write(cert_data)
            with open(key_tmp, "wb") as kf:
                kf.write(key_data)
            f.write(f"{NVS_NS_CERTS},namespace,,\n")
            f.write(f"{NVS_KEY_CERT},file,binary,{cert_tmp}\n")
            f.write(f"{NVS_KEY_KEY},file,binary,{key_tmp}\n")

        if identity:
            f.write(f"{NVS_NS_IDENTITY},namespace,,\n")
            for key, value in identity.items():
                safe_val = value.replace(",", "\\,")
                f.write(f"{key},data,string,{safe_val}\n")

    cmd = [sys.executable, nvs_gen_script, "generate",
           csv_tmp, output_bin, hex(nvs_size)]

    print(f"[*] Generating NVS binary ({nvs_size // 1024} KB)...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr, file=sys.stderr)
        die("nvs_partition_gen.py failed — see output above.")
    print(f"[+] NVS binary: {output_bin}")


def flash_nvs_binary(port, baud, nvs_address, bin_path):
    cmd = [sys.executable, "-m", "esptool",
           "--port", port, "--baud", str(baud),
           "write_flash", hex(nvs_address), bin_path]
    print(f"[*] Flashing NVS binary to {port} at {hex(nvs_address)}...")
    result = subprocess.run(cmd, capture_output=False)
    if result.returncode != 0:
        die("esptool write_flash failed — see output above.")
    print("[+] Flash complete.")


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Pothole LTE device provisioning — writes TLS creds + identity to NVS.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", required=True,
        help="Serial port (e.g. COM20 on Windows, /dev/ttyUSB0 on Linux)")

    # Cert args
    parser.add_argument("--cert", default=None, dest="cert_file",
        help="Path to device certificate PEM (.pem.crt)")
    parser.add_argument("--key", default=None, dest="key_file",
        help="Path to device private key PEM (.pem.key)")

    # Identity args
    parser.add_argument("--identity", action="store_true",
        help="Write device identity fields into NVS namespace 'fleet_id'")
    parser.add_argument("--device-id",  default=None,
        help="Device ID, e.g. pothole-unit-01")
    parser.add_argument("--org-id",     default=None,
        help="Organisation ID, e.g. ORG-001")
    parser.add_argument("--apn",        default=None,
        help="Cellular APN, e.g. airtelgprs.com")
    parser.add_argument("--endpoint",   default=None,
        help="AWS IoT Core endpoint FQDN")

    # Common
    parser.add_argument("--baud", type=int, default=921600)
    parser.add_argument("--nvs-address", type=lambda x: int(x, 0), default=0x9000)
    parser.add_argument("--nvs-size",    type=lambda x: int(x, 0), default=0x6000)
    parser.add_argument("--idf-path",    default=None)
    parser.add_argument("--dry-run",     action="store_true",
        help="Generate NVS binary but do not flash")

    args = parser.parse_args()

    has_certs    = bool(args.cert_file or args.key_file)
    has_identity = args.identity

    if not has_certs and not has_identity:
        die("Nothing to provision.  Provide --cert/--key and/or "
            "--identity with --device-id/--org-id/--apn/--endpoint.")

    # ── Read + validate certs ──────────────────────────────────────────────────
    cert_data = None
    key_data  = None

    if has_certs:
        if not args.cert_file or not args.key_file:
            die("--cert and --key must be supplied together.")
        print(f"[*] Reading certificate : {args.cert_file}")
        try:
            cert_data = open(args.cert_file, "rb").read()
        except OSError as e:
            die(f"Cannot read cert file: {e}")
        print(f"[*] Reading private key  : {args.key_file}")
        try:
            key_data = open(args.key_file, "rb").read()
        except OSError as e:
            die(f"Cannot read key file: {e}")

        validate_pem(cert_data, "-----BEGIN CERTIFICATE-----",    "Certificate")
        validate_pem(key_data,  "-----BEGIN RSA PRIVATE KEY-----","Private key")
        validate_size(cert_data, CERT_MAX_BYTES, "cert")
        validate_size(key_data,  KEY_MAX_BYTES,  "key")
        print(f"[+] Certificate : {len(cert_data)} bytes — OK")
        print(f"[+] Private key : {len(key_data)} bytes — OK")

    # ── Validate identity fields ───────────────────────────────────────────────
    identity = None

    if has_identity:
        required = {
            NVS_KEY_DEVICE_ID : args.device_id,
            NVS_KEY_ORG_ID    : args.org_id,
            NVS_KEY_APN       : args.apn,
            NVS_KEY_ENDPOINT  : args.endpoint,
        }
        missing = [k for k, v in required.items() if not v]
        if missing:
            die(f"--identity requires all identity fields. Missing: {', '.join(missing)}\n"
                f"  Provide: --device-id --org-id --apn --endpoint")
        for key, value in required.items():
            if len(value.encode()) >= IDENTITY_MAX_LEN:
                die(f"Field '{key}' exceeds max length {IDENTITY_MAX_LEN}: '{value}'")
        identity = required
        print(f"[+] Identity: device={args.device_id} org={args.org_id} "
              f"apn={args.apn}")
        print(f"[+] Endpoint: {args.endpoint}")

    # ── Find nvs_partition_gen.py ──────────────────────────────────────────────
    nvs_gen = find_nvs_partition_gen(args.idf_path)
    print(f"[+] nvs_partition_gen: {nvs_gen}")

    # ── Generate NVS binary ────────────────────────────────────────────────────
    output_bin = os.path.join(os.getcwd(), "pothole_lte_nvs.bin")
    tmpdir = tempfile.mkdtemp(prefix="pothole_prov_")

    try:
        generate_nvs_binary(cert_data, key_data, identity,
                            output_bin, args.nvs_size, nvs_gen, tmpdir)
    finally:
        for fname in os.listdir(tmpdir):
            secure_delete(os.path.join(tmpdir, fname))
        try:
            os.rmdir(tmpdir)
        except Exception:
            pass

    if args.dry_run:
        print(f"\n[*] Dry-run — NVS binary: {output_bin}")
        print("    Flash manually:")
        print(f"    esptool --port {args.port} write_flash "
              f"{hex(args.nvs_address)} {output_bin}")
        return

    # ── Flash ─────────────────────────────────────────────────────────────────
    flash_nvs_binary(args.port, args.baud, args.nvs_address, output_bin)
    secure_delete(output_bin)

    print("\n[+] Provisioning complete.")
    if has_certs:
        print(f"    Certs written to NVS namespace '{NVS_NS_CERTS}'.")
    if has_identity:
        print(f"    Identity written to NVS namespace '{NVS_NS_IDENTITY}'.")
    print("    Flash the firmware and power on the device.")


if __name__ == "__main__":
    main()
