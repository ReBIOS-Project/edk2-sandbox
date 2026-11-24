/** @file
  The local APIC of the sandbox, which has none: its one processor's APIC ID
  is 0, which exception dumps give, and nothing else of the library is there,
  so that nothing reaches for the machine's local APIC.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/LocalApicLib.h>

/**
  Get the local APIC ID of the executing processor.

  @return  0, the sandbox's one processor's.
**/
UINT32
EFIAPI
GetApicId (
  VOID
  )
{
  return 0;
}

/**
  Get the initial local APIC ID of the executing processor.

  @return  0, the sandbox's one processor's.
**/
UINT32
EFIAPI
GetInitialApicId (
  VOID
  )
{
  return 0;
}
