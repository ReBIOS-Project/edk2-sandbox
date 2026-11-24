/** @file
  The sandbox's BDS: once DXE dispatched every driver, it has the GPU's GOP
  set the mode ReBIOS's handoff names, and tells ReBIOS the result, which ends
  the sandbox's run.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Guid/EventGroup.h>
#include <Protocol/Bds.h>
#include <Protocol/DevicePath.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/PciEnumerationComplete.h>
#include <Protocol/PciIo.h>
#include <Protocol/PciRootBridgeIo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/HobLib.h>
#include <Library/HypercallLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <ReBiosSandbox.h>

STATIC CONST REBIOS_SANDBOX_HANDOFF  *mHandoff;
STATIC EFI_HANDLE                    mBdsHandle;
STATIC REBIOS_SANDBOX_MODE           mResult;

//
// The images DXE loaded before the GPU's driver, the sandbox's own, whose
// lookups that fail tell nothing of what the driver needs; the protocols the
// driver looked for in vain, each logged once; and the boot services that
// lookups go through.
//
#define MAX_OWN_IMAGES     64
#define MAX_MISSES_LOGGED  32

typedef struct {
  EFI_PHYSICAL_ADDRESS    Base;
  UINT64                  Size;
} IMAGE_RANGE;

STATIC IMAGE_RANGE               mOwnImages[MAX_OWN_IMAGES];
STATIC UINTN                     mOwnImageCount;
STATIC EFI_GUID                  mMissed[MAX_MISSES_LOGGED];
STATIC UINTN                     mMissedCount;
STATIC EFI_LOCATE_PROTOCOL       mLocateProtocol;
STATIC EFI_LOCATE_HANDLE_BUFFER  mLocateHandleBuffer;

/**
  Tells ReBIOS the task's result, which ends the sandbox's run.

  @param[in]  Status  The result.
  @param[in]  Mode    The mode set, or NULL.
**/
STATIC
VOID
Done (
  IN EFI_STATUS                 Status,
  IN CONST REBIOS_SANDBOX_MODE  *Mode OPTIONAL
  )
{
  DEBUG ((DEBUG_INFO, "ReBIOS sandbox: done: %r\n", Status));
  ReBiosHypercall (REBIOS_SANDBOX_CALL_DONE, (UINT64)Status, (UINTN)Mode, 0, 0);
  CpuDeadLoop ();
}

/**
  Starts the iGPU's GOP from the image in the handoff, so that its driver
  binding is there to connect.

  @retval EFI_SUCCESS  The driver started.
  @retval Others       It could not be loaded or started.
**/
STATIC
EFI_STATUS
StartGopImage (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  ImageHandle;

  ImageHandle = NULL;
  Status      = gBS->LoadImage (
                       FALSE,
                       gImageHandle,
                       NULL,
                       (VOID *)(UINTN)mHandoff->GopImage.Base,
                       (UINTN)mHandoff->GopImage.Size,
                       &ImageHandle
                       );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: the GOP's image does not load: %r\n", Status));
    return Status;
  }

  Status = gBS->StartImage (ImageHandle, NULL, NULL);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: the GOP's image does not start: %r\n", Status));
  }

  return Status;
}

/**
  Connects the PCI root bridges, which enumerates the GPU and the bridges
  above it, and then lets PCI configuration space reach every function.

  @retval EFI_SUCCESS    PCI is enumerated.
  @retval EFI_NOT_FOUND  There is no root bridge.
**/
STATIC
EFI_STATUS
EnumeratePci (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       HandleCount;
  UINTN       Index;
  EFI_HANDLE  Handle;

  Status = gBS->LocateHandleBuffer (
                  ByProtocol,
                  &gEfiPciRootBridgeIoProtocolGuid,
                  NULL,
                  &HandleCount,
                  &Handles
                  );
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  for (Index = 0; Index < HandleCount; Index++) {
    gBS->ConnectController (Handles[Index], NULL, NULL, FALSE);
  }

  FreePool (Handles);

  //
  // PciBusDxe announces completion only after a full enumeration, which the
  // sandbox, its resources assigned already, does not make.
  //
  Handle = NULL;
  return gBS->InstallProtocolInterface (
                &Handle,
                &gEfiPciEnumerationCompleteProtocolGuid,
                EFI_NATIVE_INTERFACE,
                NULL
                );
}

/**
  Finds the GPU's PCI I/O handle.

  @param[out]  GpuHandle  The handle.

  @retval EFI_SUCCESS    The GPU was found.
  @retval EFI_NOT_FOUND  It was not enumerated.
**/
STATIC
EFI_STATUS
FindGpu (
  OUT EFI_HANDLE  *GpuHandle
  )
{
  EFI_STATUS           Status;
  EFI_HANDLE           *Handles;
  UINTN                HandleCount;
  UINTN                Index;
  EFI_PCI_IO_PROTOCOL  *PciIo;
  UINTN                Segment;
  UINTN                Bus;
  UINTN                Device;
  UINTN                Function;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiPciIoProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    if (EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiPciIoProtocolGuid, (VOID **)&PciIo))) {
      continue;
    }

    PciIo->GetLocation (PciIo, &Segment, &Bus, &Device, &Function);
    if ((Segment == mHandoff->Gpu.Segment) && (Bus == mHandoff->Gpu.Bus) &&
        (Device == mHandoff->Gpu.Device) && (Function == mHandoff->Gpu.Function))
    {
      *GpuHandle = Handles[Index];
      Status     = EFI_SUCCESS;
      break;
    }
  }

  FreePool (Handles);
  return Status;
}

/**
  Finds the GOP on the GPU or on a child of it.

  @param[in]   GpuHandle  The GPU's handle.
  @param[out]  Gop        The GOP.

  @retval EFI_SUCCESS    The GOP was found.
  @retval EFI_NOT_FOUND  No driver produced one for the GPU.
**/
STATIC
EFI_STATUS
FindGop (
  IN  EFI_HANDLE                    GpuHandle,
  OUT EFI_GRAPHICS_OUTPUT_PROTOCOL  **Gop
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *GpuPath;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  EFI_HANDLE                *Handles;
  UINTN                     HandleCount;
  UINTN                     Index;
  UINTN                     GpuPathSize;

  GpuPath = DevicePathFromHandle (GpuHandle);
  if (GpuPath == NULL) {
    return EFI_NOT_FOUND;
  }

  GpuPathSize = GetDevicePathSize (GpuPath) - END_DEVICE_PATH_LENGTH;
  Status      = gBS->LocateHandleBuffer (ByProtocol, &gEfiGraphicsOutputProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return EFI_NOT_FOUND;
  }

  Status = EFI_NOT_FOUND;
  for (Index = 0; Index < HandleCount; Index++) {
    Path = DevicePathFromHandle (Handles[Index]);
    if ((Path != NULL) && (GetDevicePathSize (Path) >= GpuPathSize) && (CompareMem (Path, GpuPath, GpuPathSize) == 0)) {
      Status = gBS->HandleProtocol (Handles[Index], &gEfiGraphicsOutputProtocolGuid, (VOID **)Gop);
      if (!EFI_ERROR (Status)) {
        break;
      }
    }
  }

  FreePool (Handles);
  return Status;
}

/**
  Whether a mode of the GOP's is the one the handoff names.

  @param[in]  Info  The mode.

  @retval TRUE   It is.
  @retval FALSE  It is not.
**/
STATIC
BOOLEAN
IsWantedMode (
  IN CONST EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info
  )
{
  CONST REBIOS_SANDBOX_MODE  *Want;

  Want = &mHandoff->Mode;
  if ((Info->HorizontalResolution != Want->HorizontalResolution) ||
      (Info->VerticalResolution != Want->VerticalResolution) ||
      ((UINT32)Info->PixelFormat != Want->PixelFormat))
  {
    return FALSE;
  }

  if (Info->PixelFormat == PixelBitMask) {
    return (Info->PixelInformation.RedMask == Want->RedMask) &&
           (Info->PixelInformation.GreenMask == Want->GreenMask) &&
           (Info->PixelInformation.BlueMask == Want->BlueMask) &&
           (Info->PixelInformation.ReservedMask == Want->ReservedMask);
  }

  return TRUE;
}

/**
  Sets the GOP's mode that the handoff names, and fills the result with it.

  @param[in]  Gop  The GOP.

  @retval EFI_SUCCESS    The mode is set.
  @retval EFI_NOT_FOUND  The GOP has no such mode.
  @retval Others         The GOP failed to set it.
**/
STATIC
EFI_STATUS
SetWantedMode (
  IN EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop
  )
{
  EFI_STATUS                                  Status;
  UINT32                                      ModeNumber;
  UINTN                                       InfoSize;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION        *Info;
  CONST EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Set;

  for (ModeNumber = 0; ModeNumber < Gop->Mode->MaxMode; ModeNumber++) {
    if (EFI_ERROR (Gop->QueryMode (Gop, ModeNumber, &InfoSize, &Info))) {
      continue;
    }

    if (IsWantedMode (Info)) {
      FreePool (Info);
      break;
    }

    FreePool (Info);
  }

  if (ModeNumber == Gop->Mode->MaxMode) {
    DEBUG ((
      DEBUG_ERROR,
      "ReBIOS sandbox: the GOP has no %ux%u mode in format %u\n",
      mHandoff->Mode.HorizontalResolution,
      mHandoff->Mode.VerticalResolution,
      mHandoff->Mode.PixelFormat
      ));
    return EFI_NOT_FOUND;
  }

  Status = Gop->SetMode (Gop, ModeNumber);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: the GOP's mode %u does not set: %r\n", ModeNumber, Status));
    return Status;
  }

  Set                          = Gop->Mode->Info;
  mResult.HorizontalResolution = Set->HorizontalResolution;
  mResult.VerticalResolution   = Set->VerticalResolution;
  mResult.PixelFormat          = (UINT32)Set->PixelFormat;
  mResult.RedMask              = Set->PixelInformation.RedMask;
  mResult.GreenMask            = Set->PixelInformation.GreenMask;
  mResult.BlueMask             = Set->PixelInformation.BlueMask;
  mResult.ReservedMask         = Set->PixelInformation.ReservedMask;
  mResult.PixelsPerScanLine    = Set->PixelsPerScanLine;
  mResult.FrameBufferBase      = Gop->Mode->FrameBufferBase;
  DEBUG ((
    DEBUG_INFO,
    "ReBIOS sandbox: mode %u set, %ux%u, framebuffer at 0x%lx\n",
    ModeNumber,
    mResult.HorizontalResolution,
    mResult.VerticalResolution,
    mResult.FrameBufferBase
    ));
  return EFI_SUCCESS;
}

/**
  Whether code at an address is in one of the sandbox's own images.

  @param[in]  Address  The address.

  @retval TRUE   It is.
  @retval FALSE  It is in the GPU's driver, or another image loaded after
                 the sandbox's.
**/
STATIC
BOOLEAN
IsOwnCode (
  IN UINTN  Address
  )
{
  UINTN  Index;

  for (Index = 0; Index < mOwnImageCount; Index++) {
    if ((Address >= mOwnImages[Index].Base) && (Address - mOwnImages[Index].Base < mOwnImages[Index].Size)) {
      return TRUE;
    }
  }

  return FALSE;
}

/**
  Logs a protocol that the GPU's driver looked for and the sandbox lacks,
  once, which may be one the platform firmware gives it.

  @param[in]  Protocol  The protocol.
  @param[in]  Caller    Where the driver looked for it.
**/
STATIC
VOID
LogMissedProtocol (
  IN CONST EFI_GUID  *Protocol,
  IN UINTN           Caller
  )
{
  UINTN  Index;

  for (Index = 0; Index < mMissedCount; Index++) {
    if (CompareGuid (&mMissed[Index], Protocol)) {
      return;
    }
  }

  if (mMissedCount < MAX_MISSES_LOGGED) {
    CopyGuid (&mMissed[mMissedCount], Protocol);
    mMissedCount++;
  }

  DEBUG ((
    DEBUG_ERROR,
    "ReBIOS sandbox: code at 0x%lx looked for protocol %g, which the sandbox lacks\n",
    (UINT64)Caller,
    Protocol
    ));
}

/**
  LocateProtocol(), which logs a protocol that the GPU's driver looks for in
  vain.

  @param[in]   Protocol      The protocol.
  @param[in]   Registration  A registration key, or NULL.
  @param[out]  Interface     The protocol's interface.

  @return  What LocateProtocol() returns.
**/
STATIC
EFI_STATUS
EFIAPI
LoggedLocateProtocol (
  IN  EFI_GUID  *Protocol,
  IN  VOID      *Registration  OPTIONAL,
  OUT VOID      **Interface
  )
{
  UINTN       Caller;
  EFI_STATUS  Status;

  Caller = (UINTN)RETURN_ADDRESS (0);
  Status = mLocateProtocol (Protocol, Registration, Interface);
  //
  // With a registration key, a lookup asks for a protocol installed since,
  // which there is often none of.
  //
  if (EFI_ERROR (Status) && (Protocol != NULL) && (Registration == NULL) && !IsOwnCode (Caller)) {
    LogMissedProtocol (Protocol, Caller);
  }

  return Status;
}

/**
  LocateHandleBuffer(), which logs a protocol that the GPU's driver looks for
  the handles of in vain.

  @param[in]   SearchType  What to look for.
  @param[in]   Protocol    The protocol, for ByProtocol.
  @param[in]   SearchKey   The search key.
  @param[out]  NoHandles   How many handles there are.
  @param[out]  Buffer      The handles.

  @return  What LocateHandleBuffer() returns.
**/
STATIC
EFI_STATUS
EFIAPI
LoggedLocateHandleBuffer (
  IN     EFI_LOCATE_SEARCH_TYPE  SearchType,
  IN     EFI_GUID                *Protocol       OPTIONAL,
  IN     VOID                    *SearchKey      OPTIONAL,
  OUT    UINTN                   *NoHandles,
  OUT    EFI_HANDLE              **Buffer
  )
{
  UINTN       Caller;
  EFI_STATUS  Status;

  Caller = (UINTN)RETURN_ADDRESS (0);
  Status = mLocateHandleBuffer (SearchType, Protocol, SearchKey, NoHandles, Buffer);
  if (EFI_ERROR (Status) && (SearchType == ByProtocol) && (Protocol != NULL) && !IsOwnCode (Caller)) {
    LogMissedProtocol (Protocol, Caller);
  }

  return Status;
}

/**
  From now on, logs the protocols that the GPU's driver looks for and the
  sandbox lacks, as the images loaded so far are the sandbox's own. The
  lookups of an EBC driver go through the sandbox's interpreter, and are not
  logged.
**/
STATIC
VOID
WatchLookups (
  VOID
  )
{
  EFI_STATUS                 Status;
  UINTN                      HandleCount;
  EFI_HANDLE                 *Handles;
  UINTN                      Index;
  EFI_LOADED_IMAGE_PROTOCOL  *Image;

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiLoadedImageProtocolGuid, NULL, &HandleCount, &Handles);
  if (EFI_ERROR (Status)) {
    return;
  }

  for (Index = 0; Index < HandleCount && mOwnImageCount < MAX_OWN_IMAGES; Index++) {
    Status = gBS->HandleProtocol (Handles[Index], &gEfiLoadedImageProtocolGuid, (VOID **)&Image);
    if (!EFI_ERROR (Status)) {
      mOwnImages[mOwnImageCount].Base = (EFI_PHYSICAL_ADDRESS)(UINTN)Image->ImageBase;
      mOwnImages[mOwnImageCount].Size = Image->ImageSize;
      mOwnImageCount++;
    }
  }

  FreePool (Handles);
  if (Index < HandleCount) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: more images than are watched for lookups\n"));
    return;
  }

  mLocateProtocol         = gBS->LocateProtocol;
  mLocateHandleBuffer     = gBS->LocateHandleBuffer;
  gBS->LocateProtocol     = LoggedLocateProtocol;
  gBS->LocateHandleBuffer = LoggedLocateHandleBuffer;
  gBS->Hdr.CRC32          = 0;
  gBS->CalculateCrc32 (gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
}

/**
  The BDS's entry: the task of the handoff, then the end of the sandbox's run.

  @param[in]  This  The protocol.
**/
STATIC
VOID
EFIAPI
SandboxBdsEntry (
  IN EFI_BDS_ARCH_PROTOCOL  *This
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                    GpuHandle;
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;

  if (mHandoff->Task != REBIOS_SANDBOX_TASK_GOP_SET_MODE) {
    Done (EFI_UNSUPPORTED, NULL);
  }

  EfiEventGroupSignal (&gEfiEndOfDxeEventGroupGuid);
  WatchLookups ();

  if (mHandoff->GopSource == REBIOS_SANDBOX_GOP_SOURCE_IMAGE) {
    Status = StartGopImage ();
    if (EFI_ERROR (Status)) {
      Done (Status, NULL);
    }
  }

  Status = EnumeratePci ();
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: no PCI root bridge\n"));
    Done (Status, NULL);
  }

  Status = FindGpu (&GpuHandle);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: the GPU is not enumerated\n"));
    Done (Status, NULL);
  }

  gBS->ConnectController (GpuHandle, NULL, NULL, TRUE);
  Status = FindGop (GpuHandle, &Gop);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ReBIOS sandbox: no driver gives the GPU a GOP\n"));
    Done (Status, NULL);
  }

  Status = SetWantedMode (Gop);
  Done (Status, EFI_ERROR (Status) ? NULL : &mResult);
}

STATIC EFI_BDS_ARCH_PROTOCOL  mBds = {
  SandboxBdsEntry
};

/**
  Installs the BDS architectural protocol.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The protocol is installed.
  @retval Others       It could not be.
**/
EFI_STATUS
EFIAPI
SandboxBdsDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_HOB_GUID_TYPE  *GuidHob;

  GuidHob = GetFirstGuidHob (&gReBiosSandboxHandoffHobGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }

  mHandoff = GET_GUID_HOB_DATA (GuidHob);
  return gBS->InstallMultipleProtocolInterfaces (
                &mBdsHandle,
                &gEfiBdsArchProtocolGuid,
                &mBds,
                NULL
                );
}
