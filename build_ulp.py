#!/usr/bin/env python3
"""
Build script to compile ULP (Assembly) programs for ESP32.

Usage:
    python3 build_ulp.py              # Compile all .S files in src/ulp/
    python3 build_ulp.py src/ulp/ulp_blink.S

Requirements:
    - xtensa-esp32-elf-as (ESP32 toolchain)
    - xtensa-esp32-elf-ld (ESP32 toolchain)
    - xtensa-esp32-elf-objcopy (ESP32 toolchain)

Output:
    src/ulp/<name>.bin               # ULP binary (to include in firmware)
    src/ulp/<name>.o                 # Object file (intermediate)
"""

import os
import sys
import subprocess
import glob
from pathlib import Path

# Configuration
ESP_IDF_PATH = os.getenv('IDF_PATH', '/opt/esp-idf')
ULP_SRC_DIR = Path('src/ulp')
ULP_LD_SCRIPT = Path('src/ulp/esp32.ld')

# Toolchain
TOOLCHAIN_PREFIX = 'xtensa-esp32-elf-'
AS = f'{TOOLCHAIN_PREFIX}as'
LD = f'{TOOLCHAIN_PREFIX}ld'
OBJCOPY = f'{TOOLCHAIN_PREFIX}objcopy'

def find_toolchain():
    """Find the ESP32 toolchain."""
    # Try in standard PATH
    import shutil
    if shutil.which(AS):
        return True

    # Try in IDF_PATH
    idf_as = Path(ESP_IDF_PATH) / 'tools' / 'xtensa-esp32-elf' / 'bin' / AS
    if idf_as.exists():
        os.environ['PATH'] = f"{idf_as.parent}:{os.environ['PATH']}"
        return True

    return False

def create_linker_script():
    """Create ULP linker script if it doesn't exist."""
    if ULP_LD_SCRIPT.exists():
        return

    ld_content = """
MEMORY
{
  ram : org = 0x50000000, len = 0x1000
}

SECTIONS
{
  .text :
  {
    *(.text)
  } > ram
}
"""
    ULP_LD_SCRIPT.write_text(ld_content)
    print(f"[*] Linker script created: {ULP_LD_SCRIPT}")

def compile_ulp_file(asm_file):
    """Compile a single ULP Assembly file."""
    asm_path = Path(asm_file)

    if not asm_path.exists():
        print(f"[!] File not found: {asm_path}")
        return False

    if asm_path.suffix.lower() != '.s':
        print(f"[!] File is not Assembly: {asm_path}")
        return False

    obj_path = asm_path.with_suffix('.o')
    bin_path = asm_path.with_suffix('.bin')

    print(f"\n[*] Compiling: {asm_path.name}")

    # Step 1: Assemble (.S -> .o)
    cmd_as = [AS, '-o', str(obj_path), str(asm_path)]
    print(f"    Assemble: {' '.join(cmd_as)}")
    try:
        result = subprocess.run(cmd_as, capture_output=True, text=True, check=True)
        if result.stderr:
            print(f"    Warning: {result.stderr}")
    except subprocess.CalledProcessError as e:
        print(f"[!] Assemble error: {e}")
        print(f"    stderr: {e.stderr}")
        return False

    # Step 2: Link (.o -> ELF)
    elf_path = asm_path.with_suffix('.elf')
    cmd_ld = [LD, '-o', str(elf_path), str(obj_path)]
    print(f"    Link: {' '.join(cmd_ld)}")
    try:
        result = subprocess.run(cmd_ld, capture_output=True, text=True, check=True)
        if result.stderr:
            print(f"    Warning: {result.stderr}")
    except subprocess.CalledProcessError as e:
        print(f"[!] Link error: {e}")
        print(f"    stderr: {e.stderr}")
        return False

    # Step 3: Extract binary (ELF -> BIN)
    cmd_objcopy = [OBJCOPY, '-O', 'binary', str(elf_path), str(bin_path)]
    print(f"    Extract binary: {' '.join(cmd_objcopy)}")
    try:
        result = subprocess.run(cmd_objcopy, capture_output=True, text=True, check=True)
        if result.stderr:
            print(f"    Warning: {result.stderr}")
    except subprocess.CalledProcessError as e:
        print(f"[!] Objcopy error: {e}")
        print(f"    stderr: {e.stderr}")
        return False

    # Verify and print info
    bin_size = bin_path.stat().st_size
    print(f"    ✓ Binary: {bin_path.name} ({bin_size} bytes)")

    return True

def main():
    print("=" * 60)
    print("ESP32 ULP Build Helper")
    print("=" * 60)

    # Verify toolchain
    if not find_toolchain():
        print("[!] ESP32 toolchain not found!")
        print("    Install: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/")
        print("    Or, export IDF_PATH=/path/to/esp-idf")
        sys.exit(1)

    print("[✓] Toolchain found")

    # Create ULP folder if it doesn't exist
    ULP_SRC_DIR.mkdir(parents=True, exist_ok=True)

    # Create linker script
    create_linker_script()

    # Find Assembly files
    if len(sys.argv) > 1:
        # Compile specified file
        asm_files = [sys.argv[1]]
    else:
        # Compile all .S files
        asm_files = glob.glob(str(ULP_SRC_DIR / '*.S'))

    if not asm_files:
        print("[!] No Assembly files (.S) found in src/ulp/")
        print("    Create a file src/ulp/ulp_blink.S")
        sys.exit(1)

    # Compile
    success_count = 0
    for asm_file in asm_files:
        if compile_ulp_file(asm_file):
            success_count += 1

    print("\n" + "=" * 60)
    if success_count == len(asm_files):
        print(f"[✓] Build completed: {success_count}/{len(asm_files)} files compiled")
        print("\nNext steps:")
        print("  1. Add to platformio.ini:")
        print("     board_build.embed_files = src/ulp/<name>.bin")
        print("  2. Compile with: platformio run --target upload")
        sys.exit(0)
    else:
        print(f"[!] Build failed: {success_count}/{len(asm_files)} files compiled")
        sys.exit(1)

if __name__ == '__main__':
    main()
