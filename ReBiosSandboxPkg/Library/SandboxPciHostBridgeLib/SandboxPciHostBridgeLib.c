/** @file
  The sandbox's PCI root bridge: the one the GPU is under, its buses and
  apertures as ReBIOS's handoff gives them, its resources assigned already.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PciHostBridgeLib.h>
#include <Protocol/PciHostBridgeResourceAllocation.h>
#include <Protocol/PciRootBridgeIo.h>
#include <ReBiosSandbox.h>

#pragma pack(1)
typedef struct {
  ACPI_HID_DEVICE_PATH        AcpiDevicePath;
  EFI_DEVICE_PATH_PROTOCOL    EndDevicePath;
} SANDBOX_ROOT_BRIDGE_DEVICE_PATH;
#pragma pack()

#define ROOT_BRIDGE_ATTRIBUTES  (EFI_PCI_ATTRIBUTE_ISA_MOTHERBOARD_IO |     \
                                 EFI_PCI_ATTRIBUTE_ISA_IO |                 \
                                 EFI_PCI_ATTRIBUTE_ISA_IO_16 |              \
                                 EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO |         \
                                 EFI_PCI_ATTRIBUTE_VGA_PALETTE_IO_16 |      \
                                 EFI_PCI_ATTRIBUTE_VGA_MEMORY |             \
                                 EFI_PCI_ATTRIBUTE_VGA_IO |                 \
                                 EFI_PCI_ATTRIBUTE_VGA_IO_16)

/**
  An aperture from a range of the handoff, none where its size is 0.

  @param[out]  Aperture  The aperture.
  @param[in]   Range     The range.
**/
STATIC
VOID
SetAperture (
  OUT PCI_ROOT_BRIDGE_APERTURE   *Aperture,
  IN CONST REBIOS_SANDBOX_RANGE  *Range
  )
{
  if (Range->Size == 0) {
    Aperture->Base  = MAX_UINT64;
    Aperture->Limit = 0;
  } else {
    Aperture->Base  = Range->Base;
    Aperture->Limit = Range->Base + Range->Size - 1;
  }

  Aperture->Translation = 0;
}

/**
  Returns the sandbox's one root bridge.

  @param[out]  Count  The number of root bridges, 1, or 0 without a handoff.

  @return  The root bridge.
**/
PCI_ROOT_BRIDGE *
EFIAPI
PciHostBridgeGetRootBridges (
  UINTN  *Count
  )
{
  EFI_HOB_GUID_TYPE                *GuidHob;
  CONST REBIOS_SANDBOX_HANDOFF     *Handoff;
  PCI_ROOT_BRIDGE                  *Bridge;
  SANDBOX_ROOT_BRIDGE_DEVICE_PATH  *DevicePath;

  *Count  = 0;
  GuidHob = GetFirstGuidHob (&gReBiosSandboxHandoffHobGuid);
  if (GuidHob == NULL) {
    return NULL;
  }

  Handoff    = GET_GUID_HOB_DATA (GuidHob);
  Bridge     = AllocateZeroPool (sizeof (PCI_ROOT_BRIDGE));
  DevicePath = AllocateZeroPool (sizeof (SANDBOX_ROOT_BRIDGE_DEVICE_PATH));
  if ((Bridge == NULL) || (DevicePath == NULL)) {
    return NULL;
  }

  Bridge->Segment               = Handoff->Gpu.Segment;
  Bridge->Supports              = ROOT_BRIDGE_ATTRIBUTES;
  Bridge->Attributes            = ROOT_BRIDGE_ATTRIBUTES;
  Bridge->DmaAbove4G            = TRUE;
  Bridge->NoExtendedConfigSpace = FALSE;
  Bridge->ResourceAssigned      = TRUE;
  Bridge->AllocationAttributes  = EFI_PCI_HOST_BRIDGE_COMBINE_MEM_PMEM;
  if (Handoff->Mem64Aperture.Size != 0) {
    Bridge->AllocationAttributes |= EFI_PCI_HOST_BRIDGE_MEM64_DECODE;
  }

  Bridge->Bus.Base        = Handoff->RootBusBase;
  Bridge->Bus.Limit       = Handoff->RootBusLimit;
  Bridge->Bus.Translation = 0;
  SetAperture (&Bridge->Io, &Handoff->IoAperture);
  SetAperture (&Bridge->Mem, &Handoff->Mem32Aperture);
  SetAperture (&Bridge->MemAbove4G, &Handoff->Mem64Aperture);
  Bridge->PMem.Base         = MAX_UINT64;
  Bridge->PMem.Limit        = 0;
  Bridge->PMemAbove4G.Base  = MAX_UINT64;
  Bridge->PMemAbove4G.Limit = 0;

  DevicePath->AcpiDevicePath.Header.Type    = ACPI_DEVICE_PATH;
  DevicePath->AcpiDevicePath.Header.SubType = ACPI_DP;
  SetDevicePathNodeLength (&DevicePath->AcpiDevicePath.Header, sizeof (ACPI_HID_DEVICE_PATH));
  DevicePath->AcpiDevicePath.HID = EISA_PNP_ID (0x0A03);
  DevicePath->AcpiDevicePath.UID = Handoff->RootBusBase;
  SetDevicePathEndNode (&DevicePath->EndDevicePath);
  Bridge->DevicePath = (EFI_DEVICE_PATH_PROTOCOL *)DevicePath;

  *Count = 1;
  return Bridge;
}

/**
  Frees the root bridges PciHostBridgeGetRootBridges () returned.

  @param[in]  Bridges  The root bridges.
  @param[in]  Count    Their number.
**/
VOID
EFIAPI
PciHostBridgeFreeRootBridges (
  PCI_ROOT_BRIDGE  *Bridges,
  UINTN            Count
  )
{
  UINTN  Index;

  for (Index = 0; Index < Count; Index++) {
    FreePool (Bridges[Index].DevicePath);
  }

  if (Bridges != NULL) {
    FreePool (Bridges);
  }
}

/**
  Reports a resource conflict, which the sandbox cannot have, as its
  resources are assigned already.

  @param[in]  HostBridgeHandle  The host bridge.
  @param[in]  Configuration     The resources.
**/
VOID
EFIAPI
PciHostBridgeResourceConflict (
  EFI_HANDLE  HostBridgeHandle,
  VOID        *Configuration
  )
{
  DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: PCI resource conflict\n"));
}
