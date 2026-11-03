#!/usr/bin/env python3
"""Record the honest native QRX68100 AROS boot readiness (not FS-UAE).
Fail-closed evidence gate; status remains blocked until genuine guest boot and
AROS BSP/driver tests are supplied. External host GUI is never counted as AROS.
"""
import argparse
import datetime
import json
from pathlib import Path


def evaluate(root: Path):
    musashi=(root/'engine/third_party/Musashi/m68kcpu.c').is_file()
    # A driver is only marked ready by verified native integration; a C header,
    # host-only renderer or FS-UAE screenshot is never enough.
    checks={
        'musashi_68040_source_installed': musashi,
        '68040_fpu_exception_differential_test_passed': False,
        '68040_mmu_guest_address_translation_qualified': False,
        'aros_qrx_board_support_package_built_and_booted': False,
        'guest_aros_qrxhostfs_source_present': (root/'engine/guest/aros/bsp/QRXShare.c').is_file(),
        'guest_aros_qrx_board_smp_hooks_source_present': (root/'engine/guest/aros/bsp/qrx_smp_board.h').is_file(),
        'portable_shared_cpu_affinity_queue_source_present': (root/'engine/bsp/qrx_smp_ready.c').is_file(),
        'aros_m68k_exec_smp_scheduler_linked': False,
        'arOS_dos_host_filesystem_handler_mounted': False,
        'aros_native_qrxgpu_rtg_driver_qualified': False,
        'aros_native_ahi_and_input_drivers_qualified': False,
        'aros_block_device_dos_filesystem_driver_qualified': False,
        'qrxdesktop_native_guest_process_started': False,
        'qrx86_amiga_guest_service_verified': False,
        'native_uefi_or_boot_iso_verified': False,
    }
    return {'project':'ARMITHLON QRX OS One','native_aros_on_qrx68100':'BLOCKED',
            'checks':checks,'separate_fsuae_compatibility_mode':'NOT COUNTED',
            'available_board_feature':'raw 512-byte DMA disk + sandboxed host-directory MMIO + dual 68040 contexts (not yet native AROS)',
            'next_test':'Cross-compile QRXShare/QRXBoardInfo; implement AROS DOS packet handler and Exec SMP target adaptation; boot AROS on Musashi and require QRXDesktop guest window',
            'provenance':'source readiness gate, not a machine test'}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--output',type=Path)
    a=p.parse_args()
    result=evaluate(a.root)
    data=json.dumps(result,indent=2,sort_keys=True)+'\n'
    if a.output:
        a.output.parent.mkdir(parents=True,exist_ok=True)
        a.output.write_text(data)
    print(data,end='')


if __name__=='__main__':main()
