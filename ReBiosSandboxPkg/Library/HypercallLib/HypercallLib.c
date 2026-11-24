/** @file
  Hypercalls from the sandbox firmware to ReBIOS, through VMCALL on Intel
  processors and VMMCALL on AMD ones.

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Base.h>
#include <Library/BaseLib.h>
#include <Library/HypercallLib.h>
#include <Register/Amd/Cpuid.h>
#include <Register/Intel/Cpuid.h>

UINT64
EFIAPI
AsmReBiosVmcall (
  IN UINT64  Call,
  IN UINT64  Arg1,
  IN UINT64  Arg2,
  IN UINT64  Arg3,
  IN UINT64  Arg4
  );

UINT64
EFIAPI
AsmReBiosVmmcall (
  IN UINT64  Call,
  IN UINT64  Arg1,
  IN UINT64  Arg2,
  IN UINT64  Arg3,
  IN UINT64  Arg4
  );

//
// Whether the processor is AMD's, once known: CPUID makes an exit in the
// sandbox, so it is asked once.
//
STATIC BOOLEAN  mVendorKnown;
STATIC BOOLEAN  mAmd;

/**
  Makes a hypercall to ReBIOS, through the instruction of the processor's
  vendor.

  @param[in]  Call  The hypercall's number, REBIOS_SANDBOX_CALL_*.
  @param[in]  Arg1  Its first argument, in RBX.
  @param[in]  Arg2  Its second argument, in RCX.
  @param[in]  Arg3  Its third argument, in RDX.
  @param[in]  Arg4  Its fourth argument, in RSI.

  @return  What the hypercall returns in RAX.
**/
UINT64
EFIAPI
ReBiosHypercall (
  IN UINT64  Call,
  IN UINT64  Arg1,
  IN UINT64  Arg2,
  IN UINT64  Arg3,
  IN UINT64  Arg4
  )
{
  UINT32  Ebx;
  UINT32  Ecx;
  UINT32  Edx;

  if (!mVendorKnown) {
    AsmCpuid (CPUID_SIGNATURE, NULL, &Ebx, &Ecx, &Edx);
    mAmd = (Ebx == CPUID_SIGNATURE_AUTHENTIC_AMD_EBX) &&
           (Ecx == CPUID_SIGNATURE_AUTHENTIC_AMD_ECX) &&
           (Edx == CPUID_SIGNATURE_AUTHENTIC_AMD_EDX);
    mVendorKnown = TRUE;
  }

  if (mAmd) {
    return AsmReBiosVmmcall (Call, Arg1, Arg2, Arg3, Arg4);
  }

  return AsmReBiosVmcall (Call, Arg1, Arg2, Arg3, Arg4);
}
