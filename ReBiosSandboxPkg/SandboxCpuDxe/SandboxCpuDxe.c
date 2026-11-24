/** @file
  The sandbox's CPU architectural protocol, for the one processor the sandbox
  has: its interrupts, its IDT, and the TSC as its timer. Memory attributes
  stay as ReBIOS's page tables set them, and an exception ends the sandbox's
  run as a failure.

  The IDT's growth to 256 entries is based on UefiCpuPkg/CpuDxe.

  Copyright (c) 2008 - 2023, Intel Corporation. All rights reserved.<BR>
  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Protocol/Cpu.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CpuExceptionHandlerLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/HypercallLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <ReBiosSandbox.h>

#define INTERRUPT_COUNT          256
#define EXCEPTION_COUNT          32
#define FEMTOSECONDS_PER_SECOND  1000000000000000ULL

STATIC UINT64      mTscFrequency;
STATIC EFI_HANDLE  mCpuHandle;

/**
  Flushes a range of the processor's data cache, the whole of it, as WBINVD
  does.

  @param[in]  This       The protocol.
  @param[in]  Start      The range's start.
  @param[in]  Length     Its length.
  @param[in]  FlushType  The flush's type.

  @retval EFI_SUCCESS      The cache was written back and invalidated.
  @retval EFI_UNSUPPORTED  An invalidation alone, which would lose data.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxFlushCpuDataCache (
  IN EFI_CPU_ARCH_PROTOCOL  *This,
  IN EFI_PHYSICAL_ADDRESS   Start,
  IN UINT64                 Length,
  IN EFI_CPU_FLUSH_TYPE     FlushType
  )
{
  if (FlushType == EfiCpuFlushTypeInvalidate) {
    return EFI_UNSUPPORTED;
  }

  AsmWbinvd ();
  return EFI_SUCCESS;
}

/**
  Enables interrupts.

  @param[in]  This  The protocol.

  @retval EFI_SUCCESS  Interrupts are on.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxEnableInterrupt (
  IN EFI_CPU_ARCH_PROTOCOL  *This
  )
{
  EnableInterrupts ();
  return EFI_SUCCESS;
}

/**
  Disables interrupts.

  @param[in]  This  The protocol.

  @retval EFI_SUCCESS  Interrupts are off.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxDisableInterrupt (
  IN EFI_CPU_ARCH_PROTOCOL  *This
  )
{
  DisableInterrupts ();
  return EFI_SUCCESS;
}

/**
  Returns whether interrupts are on.

  @param[in]   This   The protocol.
  @param[out]  State  Whether they are.

  @retval EFI_SUCCESS            State was set.
  @retval EFI_INVALID_PARAMETER  State is NULL.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxGetInterruptState (
  IN  EFI_CPU_ARCH_PROTOCOL  *This,
  OUT BOOLEAN                *State
  )
{
  if (State == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *State = GetInterruptState ();
  return EFI_SUCCESS;
}

/**
  Sends an INIT, which the sandbox does not.

  @param[in]  This      The protocol.
  @param[in]  InitType  The INIT's type.

  @retval EFI_UNSUPPORTED  Always.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxInit (
  IN EFI_CPU_ARCH_PROTOCOL  *This,
  IN EFI_CPU_INIT_TYPE      InitType
  )
{
  return EFI_UNSUPPORTED;
}

/**
  Registers a handler for an interrupt or exception.

  @param[in]  This              The protocol.
  @param[in]  InterruptType     The vector.
  @param[in]  InterruptHandler  The handler, or NULL to remove one.

  @return  What the exception handler library returns.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxRegisterInterruptHandler (
  IN EFI_CPU_ARCH_PROTOCOL      *This,
  IN EFI_EXCEPTION_TYPE         InterruptType,
  IN EFI_CPU_INTERRUPT_HANDLER  InterruptHandler
  )
{
  return RegisterCpuInterruptHandler (InterruptType, InterruptHandler);
}

/**
  Returns the TSC and its period.

  @param[in]   This         The protocol.
  @param[in]   TimerIndex   The timer, 0, the only one.
  @param[out]  TimerValue   Its value.
  @param[out]  TimerPeriod  Its period in femtoseconds, optional.

  @retval EFI_SUCCESS            The value was returned.
  @retval EFI_INVALID_PARAMETER  TimerValue is NULL, or TimerIndex is not 0.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxGetTimerValue (
  IN  EFI_CPU_ARCH_PROTOCOL  *This,
  IN  UINT32                 TimerIndex,
  OUT UINT64                 *TimerValue,
  OUT UINT64                 *TimerPeriod OPTIONAL
  )
{
  if ((TimerValue == NULL) || (TimerIndex != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  *TimerValue = AsmReadTsc ();
  if (TimerPeriod != NULL) {
    *TimerPeriod = DivU64x64Remainder (FEMTOSECONDS_PER_SECOND, mTscFrequency, NULL);
  }

  return EFI_SUCCESS;
}

/**
  Sets the attributes of a range of memory, which stay as ReBIOS's page
  tables have them in the sandbox.

  @param[in]  This        The protocol.
  @param[in]  BaseAddress  The range's start.
  @param[in]  Length       Its length.
  @param[in]  Attributes   The attributes.

  @retval EFI_SUCCESS  Always.
**/
STATIC
EFI_STATUS
EFIAPI
SandboxSetMemoryAttributes (
  IN EFI_CPU_ARCH_PROTOCOL  *This,
  IN EFI_PHYSICAL_ADDRESS   BaseAddress,
  IN UINT64                 Length,
  IN UINT64                 Attributes
  )
{
  return EFI_SUCCESS;
}

STATIC EFI_CPU_ARCH_PROTOCOL  mCpu = {
  SandboxFlushCpuDataCache,
  SandboxEnableInterrupt,
  SandboxDisableInterrupt,
  SandboxGetInterruptState,
  SandboxInit,
  SandboxRegisterInterruptHandler,
  SandboxGetTimerValue,
  SandboxSetMemoryAttributes,
  1,
  4
};

/**
  Ends the sandbox's run at an exception, once its context is in the log.

  @param[in]  ExceptionType  The exception.
  @param[in]  SystemContext  The processor's context at it.
**/
STATIC
VOID
EFIAPI
SandboxException (
  IN EFI_EXCEPTION_TYPE  ExceptionType,
  IN EFI_SYSTEM_CONTEXT  SystemContext
  )
{
  DumpCpuContext (ExceptionType, SystemContext);
  ReBiosHypercall (REBIOS_SANDBOX_CALL_DONE, (UINT64)EFI_ABORTED, 0, 0, 0);
  CpuDeadLoop ();
}

/**
  Gives the IDT room for every vector, the exceptions their handlers, and
  installs the CPU architectural protocol.

  @param[in]  ImageHandle  The image handle.
  @param[in]  SystemTable  The system table.

  @retval EFI_SUCCESS  The protocol is installed.
  @retval Others       It could not be.
**/
EFI_STATUS
EFIAPI
SandboxCpuDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_HOB_GUID_TYPE             *GuidHob;
  CONST REBIOS_SANDBOX_HANDOFF  *Handoff;
  IA32_DESCRIPTOR               IdtDescriptor;
  IA32_IDT_GATE_DESCRIPTOR      *IdtTable;
  UINTN                         IdtEntryCount;
  EFI_EXCEPTION_TYPE            Exception;

  GuidHob = GetFirstGuidHob (&gReBiosSandboxHandoffHobGuid);
  if (GuidHob == NULL) {
    return EFI_NOT_FOUND;
  }

  Handoff       = GET_GUID_HOB_DATA (GuidHob);
  mTscFrequency = Handoff->TscFrequency;

  AsmReadIdtr (&IdtDescriptor);
  IdtEntryCount = (IdtDescriptor.Limit + 1) / sizeof (IA32_IDT_GATE_DESCRIPTOR);
  if (IdtEntryCount < INTERRUPT_COUNT) {
    IdtTable = AllocateZeroPool (sizeof (IA32_IDT_GATE_DESCRIPTOR) * INTERRUPT_COUNT);
    if (IdtTable == NULL) {
      return EFI_OUT_OF_RESOURCES;
    }

    if ((IdtDescriptor.Base != 0) && (IdtEntryCount != 0)) {
      CopyMem (IdtTable, (VOID *)IdtDescriptor.Base, sizeof (IA32_IDT_GATE_DESCRIPTOR) * IdtEntryCount);
    }

    IdtDescriptor.Base  = (UINTN)IdtTable;
    IdtDescriptor.Limit = (UINT16)(sizeof (IA32_IDT_GATE_DESCRIPTOR) * INTERRUPT_COUNT - 1);
    AsmWriteIdtr (&IdtDescriptor);
  }

  Status = InitializeCpuExceptionHandlers (NULL);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  for (Exception = 0; Exception < EXCEPTION_COUNT; Exception++) {
    RegisterCpuInterruptHandler (Exception, SandboxException);
  }

  return gBS->InstallMultipleProtocolInterfaces (
                &mCpuHandle,
                &gEfiCpuArchProtocolGuid,
                &mCpu,
                NULL
                );
}
