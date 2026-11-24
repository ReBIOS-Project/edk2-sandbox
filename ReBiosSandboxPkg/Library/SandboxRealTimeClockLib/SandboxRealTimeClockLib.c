/** @file
  The sandbox's real time clock: the platform's time, which ReBIOS gives
  through a hypercall. The sandbox sets neither the time nor a wakeup.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/HypercallLib.h>
#include <Library/RealTimeClockLib.h>
#include <ReBiosSandbox.h>

/**
  Returns the current time and date information, and the time-keeping
  capabilities of the hardware platform.

  @param[out]  Time          The current time.
  @param[out]  Capabilities  The time-keeping capabilities, optional.

  @retval EFI_SUCCESS            The operation completed successfully.
  @retval EFI_INVALID_PARAMETER  Time is NULL.
**/
EFI_STATUS
EFIAPI
LibGetTime (
  OUT EFI_TIME               *Time,
  OUT EFI_TIME_CAPABILITIES  *Capabilities
  )
{
  if (Time == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  ReBiosHypercall (REBIOS_SANDBOX_CALL_GET_TIME, (UINTN)Time, 0, 0, 0);
  if (Capabilities != NULL) {
    Capabilities->Resolution = 1;
    Capabilities->Accuracy   = 50000000;
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

/**
  Sets the current local time and date information, which the sandbox does
  not do.

  @param[in]  Time  A pointer to the current time.

  @retval EFI_UNSUPPORTED  Always.
**/
EFI_STATUS
EFIAPI
LibSetTime (
  IN EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Returns the current wakeup alarm clock setting, which the sandbox does not
  have.

  @param[out]  Enabled  Indicates if the alarm is currently enabled.
  @param[out]  Pending  Indicates if the alarm signal is pending.
  @param[out]  Time     The current alarm setting.

  @retval EFI_UNSUPPORTED  Always.
**/
EFI_STATUS
EFIAPI
LibGetWakeupTime (
  OUT BOOLEAN   *Enabled,
  OUT BOOLEAN   *Pending,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Sets the system wakeup alarm clock time, which the sandbox does not do.

  @param[in]   Enabled  Enable or disable the wakeup alarm.
  @param[out]  Time     The time to set the alarm for.

  @retval EFI_UNSUPPORTED  Always.
**/
EFI_STATUS
EFIAPI
LibSetWakeupTime (
  IN BOOLEAN    Enabled,
  OUT EFI_TIME  *Time
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Initializes the real time clock, which needs nothing.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  Always.
**/
EFI_STATUS
EFIAPI
LibRtcInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  return EFI_SUCCESS;
}
