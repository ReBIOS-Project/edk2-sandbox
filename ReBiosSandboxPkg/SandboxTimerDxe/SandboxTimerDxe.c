/** @file
  The sandbox's timer architectural protocol: ReBIOS raises its interrupt at
  the period the firmware asks for through a hypercall, and the interrupt needs
  no end of interrupt.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Protocol/Cpu.h>
#include <Protocol/Timer.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/HypercallLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <ReBiosSandbox.h>

//
// 10 ms, in 100 ns units, until DxeCore asks for another.
//
#define DEFAULT_TIMER_PERIOD  100000

STATIC EFI_TIMER_NOTIFY  mTimerNotifyFunction;
STATIC UINT64            mTimerPeriod;
STATIC EFI_HANDLE        mTimerHandle;

/**
  The timer's interrupt: the time that went by goes to DxeCore's handler.

  @param[in]  InterruptType  The vector.
  @param[in]  SystemContext  The processor's context.
**/
STATIC
VOID
EFIAPI
TimerInterruptHandler (
  IN EFI_EXCEPTION_TYPE  InterruptType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  EFI_TPL  OriginalTpl;

  OriginalTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (mTimerNotifyFunction != NULL) {
    mTimerNotifyFunction (mTimerPeriod);
  }

  gBS->RestoreTPL (OriginalTpl);
}

/**
  Registers the handler the timer's interrupt calls.

  @param[in]  This            The protocol.
  @param[in]  NotifyFunction  The handler, or NULL to remove it.

  @retval EFI_SUCCESS            The handler was registered or removed.
  @retval EFI_ALREADY_STARTED    A handler was registered already.
  @retval EFI_INVALID_PARAMETER  There is no handler to remove.
**/
STATIC
EFI_STATUS
EFIAPI
TimerRegisterHandler (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN EFI_TIMER_NOTIFY         NotifyFunction
  )
{
  if ((NotifyFunction == NULL) && (mTimerNotifyFunction == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((NotifyFunction != NULL) && (mTimerNotifyFunction != NULL)) {
    return EFI_ALREADY_STARTED;
  }

  mTimerNotifyFunction = NotifyFunction;
  return EFI_SUCCESS;
}

/**
  Sets the timer's period, which ReBIOS raises its interrupt at.

  @param[in]  This         The protocol.
  @param[in]  TimerPeriod  The period in 100 ns units, 0 for none.

  @retval EFI_SUCCESS  The period was set.
**/
STATIC
EFI_STATUS
EFIAPI
TimerSetTimerPeriod (
  IN EFI_TIMER_ARCH_PROTOCOL  *This,
  IN UINT64                   TimerPeriod
  )
{
  ReBiosHypercall (REBIOS_SANDBOX_CALL_TIMER, TimerPeriod, 0, 0, 0);
  mTimerPeriod = TimerPeriod;
  return EFI_SUCCESS;
}

/**
  Returns the timer's period.

  @param[in]   This         The protocol.
  @param[out]  TimerPeriod  The period in 100 ns units.

  @retval EFI_SUCCESS            The period was returned.
  @retval EFI_INVALID_PARAMETER  TimerPeriod is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
TimerGetTimerPeriod (
  IN  EFI_TIMER_ARCH_PROTOCOL  *This,
  OUT UINT64                   *TimerPeriod
  )
{
  if (TimerPeriod == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *TimerPeriod = mTimerPeriod;
  return EFI_SUCCESS;
}

/**
  Calls the timer's handler as its interrupt would.

  @param[in]  This  The protocol.

  @retval EFI_SUCCESS  The handler was called, where there is one.
**/
STATIC
EFI_STATUS
EFIAPI
TimerGenerateSoftInterrupt (
  IN EFI_TIMER_ARCH_PROTOCOL  *This
  )
{
  EFI_TPL  OriginalTpl;

  OriginalTpl = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  if (mTimerNotifyFunction != NULL) {
    mTimerNotifyFunction (0);
  }

  gBS->RestoreTPL (OriginalTpl);
  return EFI_SUCCESS;
}

STATIC EFI_TIMER_ARCH_PROTOCOL  mTimer = {
  TimerRegisterHandler,
  TimerSetTimerPeriod,
  TimerGetTimerPeriod,
  TimerGenerateSoftInterrupt
};

/**
  Takes the timer's vector, starts the timer, and installs the timer
  architectural protocol.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The protocol is installed.
  @retval Others       It could not be.
**/
EFI_STATUS
EFIAPI
SandboxTimerDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS             Status;
  EFI_CPU_ARCH_PROTOCOL  *Cpu;

  Status = gBS->LocateProtocol (&gEfiCpuArchProtocolGuid, NULL, (VOID **)&Cpu);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = Cpu->RegisterInterruptHandler (Cpu, REBIOS_SANDBOX_TIMER_VECTOR, TimerInterruptHandler);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  TimerSetTimerPeriod (&mTimer, DEFAULT_TIMER_PERIOD);

  return gBS->InstallMultipleProtocolInterfaces (
                &mTimerHandle,
                &gEfiTimerArchProtocolGuid,
                &mTimer,
                NULL
                );
}
