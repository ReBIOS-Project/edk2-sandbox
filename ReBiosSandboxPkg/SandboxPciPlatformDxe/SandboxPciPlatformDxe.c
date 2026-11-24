/** @file
  The sandbox's PCI platform protocol: the GPU's option ROM is the image the
  platform firmware's PCI I/O protocol gave it, from ReBIOS's handoff, so that
  nothing in the sandbox reads the card's ROM BAR.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciPlatform.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <ReBiosSandbox.h>

STATIC CONST REBIOS_SANDBOX_HANDOFF  *mHandoff;

/**
  Hears of the host bridge's resource allocation phases, which need nothing.

  @param[in]  This        The protocol.
  @param[in]  HostBridge  The host bridge.
  @param[in]  Phase       The phase.
  @param[in]  ExecPhase   Before or after it.

  @retval EFI_UNSUPPORTED  Always.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxPlatformNotify (
  IN EFI_PCI_PLATFORM_PROTOCOL                      *This,
  IN EFI_HANDLE                                     HostBridge,
  IN EFI_PCI_HOST_BRIDGE_RESOURCE_ALLOCATION_PHASE  Phase,
  IN EFI_PCI_EXECUTION_PHASE                        ExecPhase
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Hears of a controller's resource allocation phases, which need nothing.

  @param[in]  This        The protocol.
  @param[in]  HostBridge  The host bridge.
  @param[in]  RootBridge  The root bridge.
  @param[in]  PciAddress  The controller.
  @param[in]  Phase       The phase.
  @param[in]  ExecPhase   Before or after it.

  @retval EFI_UNSUPPORTED  Always.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxPlatformPrepController (
  IN EFI_PCI_PLATFORM_PROTOCOL                     *This,
  IN EFI_HANDLE                                    HostBridge,
  IN EFI_HANDLE                                    RootBridge,
  IN EFI_PCI_ROOT_BRIDGE_IO_PROTOCOL_PCI_ADDRESS   PciAddress,
  IN EFI_PCI_CONTROLLER_RESOURCE_ALLOCATION_PHASE  Phase,
  IN EFI_PCI_EXECUTION_PHASE                       ExecPhase
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Returns the platform's PCI policy, which has nothing particular.

  @param[in]   This       The protocol.
  @param[out]  PciPolicy  The policy.

  @retval EFI_SUCCESS            The policy was returned.
  @retval EFI_INVALID_PARAMETER  PciPolicy is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxGetPlatformPolicy (
  IN  CONST EFI_PCI_PLATFORM_PROTOCOL  *This,
  OUT EFI_PCI_PLATFORM_POLICY          *PciPolicy
  )
{
  if (PciPolicy == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *PciPolicy = 0;
  return EFI_SUCCESS;
}

/**
  Returns the option ROM of a PCI function: the GPU's, from the handoff, the
  one its GOP comes from, or else the one the platform firmware gave it.

  @param[in]   This       The protocol.
  @param[in]   PciHandle  The function's handle.
  @param[out]  RomImage   The ROM's image.
  @param[out]  RomSize    Its size.

  @retval EFI_SUCCESS            The GPU's ROM was returned.
  @retval EFI_NOT_FOUND          The function is not the GPU, or the handoff
                                 gives no ROM of it.
  @retval EFI_INVALID_PARAMETER  RomImage or RomSize is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxGetPciRom (
  IN  CONST EFI_PCI_PLATFORM_PROTOCOL  *This,
  IN        EFI_HANDLE                 PciHandle,
  OUT       VOID                       **RomImage,
  OUT       UINTN                      *RomSize
  )
{
  EFI_STATUS                  Status;
  CONST REBIOS_SANDBOX_RANGE  *Rom;
  EFI_PCI_IO_PROTOCOL         *PciIo;
  UINTN                       Segment;
  UINTN                       Bus;
  UINTN                       Device;
  UINTN                       Function;

  if ((RomImage == NULL) || (RomSize == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *RomImage = NULL;
  *RomSize  = 0;
  if ((mHandoff->GopSource == REBIOS_SANDBOX_GOP_SOURCE_OPTION_ROM) && (mHandoff->GopImage.Size != 0)) {
    Rom = &mHandoff->GopImage;
  } else if (mHandoff->Rom.Size != 0) {
    Rom = &mHandoff->Rom;
  } else {
    return EFI_NOT_FOUND;
  }

  Status = gBS->HandleProtocol (PciHandle, &gEfiPciIoProtocolGuid, (VOID **)&PciIo);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  PciIo->GetLocation (PciIo, &Segment, &Bus, &Device, &Function);
  if ((Segment != mHandoff->Gpu.Segment) || (Bus != mHandoff->Gpu.Bus) ||
      (Device != mHandoff->Gpu.Device) || (Function != mHandoff->Gpu.Function))
  {
    return EFI_NOT_FOUND;
  }

  *RomImage = (VOID *)(UINTN)Rom->Base;
  *RomSize  = (UINTN)Rom->Size;
  return EFI_SUCCESS;
}

STATIC EFI_PCI_PLATFORM_PROTOCOL  mPciPlatform = {
  SandboxPlatformNotify,
  SandboxPlatformPrepController,
  SandboxGetPlatformPolicy,
  SandboxGetPciRom
};

/**
  Installs the PCI platform protocol.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The protocol is installed.
  @retval Others       It could not be.
**/
EFI_STATUS
EFIAPI
SandboxPciPlatformDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *GuidHob;
  EFI_HANDLE         Handle;

  GuidHob = GetFirstGuidHob (&gReBiosSandboxHandoffHobGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }

  mHandoff = GET_GUID_HOB_DATA (GuidHob);
  Handle   = NULL;
  return gBS->InstallMultipleProtocolInterfaces (
                &Handle,
                &gEfiPciPlatformProtocolGuid,
                &mPciPlatform,
                NULL
                );
}
