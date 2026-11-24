/** @file
  Resets in the sandbox, which end its run as a failure, as the firmware
  cannot reset the machine.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/HypercallLib.h>
#include <Library/ResetSystemLib.h>
#include <ReBiosSandbox.h>

/**
  Asks ReBIOS for a reset, which ends the sandbox's run.

  @param[in]  ResetType  The reset's type.
**/
STATIC
VOID
SandboxReset (
  IN EFI_RESET_TYPE  ResetType
  )
{
  ReBiosHypercall (REBIOS_SANDBOX_CALL_RESET, ResetType, 0, 0, 0);
  CpuDeadLoop ();
}

/**
  This function causes a system-wide reset (cold reset), which ends the
  sandbox's run.
**/
VOID
EFIAPI
ResetCold (
  VOID
  )
{
  SandboxReset (EfiResetCold);
}

/**
  This function causes a system-wide initialization (warm reset), which ends
  the sandbox's run.
**/
VOID
EFIAPI
ResetWarm (
  VOID
  )
{
  SandboxReset (EfiResetWarm);
}

/**
  This function causes the system to enter a power state equivalent to the
  ACPI G2/S5 or G3 states, which ends the sandbox's run.
**/
VOID
EFIAPI
ResetShutdown (
  VOID
  )
{
  SandboxReset (EfiResetShutdown);
}

/**
  This function causes a systemwide reset of a platform-specific type, which
  ends the sandbox's run.

  @param[in]  DataSize   The size, in bytes, of ResetData.
  @param[in]  ResetData  A Null-terminated string, followed by the EFI_GUID.
**/
VOID
EFIAPI
ResetPlatformSpecific (
  IN UINTN  DataSize,
  IN VOID   *ResetData
  )
{
  SandboxReset (EfiResetPlatformSpecific);
}

/**
  The ResetSystem function resets the entire platform, which ends the
  sandbox's run.

  @param[in]  ResetType    The type of reset to perform.
  @param[in]  ResetStatus  The status code for the reset.
  @param[in]  DataSize     The size, in bytes, of ResetData.
  @param[in]  ResetData    A Null-terminated description, optionally followed
                           by binary data.
**/
VOID
EFIAPI
ResetSystem (
  IN EFI_RESET_TYPE  ResetType,
  IN EFI_STATUS      ResetStatus,
  IN UINTN           DataSize,
  IN VOID            *ResetData OPTIONAL
  )
{
  SandboxReset (ResetType);
}
