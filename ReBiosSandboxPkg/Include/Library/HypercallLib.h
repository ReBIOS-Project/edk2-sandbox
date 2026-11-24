/** @file
  Hypercalls from the sandbox firmware to ReBIOS (ReBiosSandbox.h).

  Copyright (c) 2026, Mintsuki and contributors.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef HYPERCALL_LIB_H_
#define HYPERCALL_LIB_H_

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
  );

#endif
