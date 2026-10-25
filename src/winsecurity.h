#ifndef OPENPILOT_WINSECURITY_H
#define OPENPILOT_WINSECURITY_H

#include "internal.h"

/* Overwrite retains the destination DACL, as an in-place Windows copy did.
 * Applying it to the completed staging object cannot weaken the live target
 * on cancellation or failed publication. Already granted handle rights remain. */
static DWORD win_copy_dacl(HANDLE previous,HANDLE replacement)
{
    DWORD n=0;GetKernelObjectSecurity(previous,DACL_SECURITY_INFORMATION,NULL,0,&n);
    if(!n)return GetLastError();PSECURITY_DESCRIPTOR sd=fpe_alloc(n);if(!sd)return ERROR_NOT_ENOUGH_MEMORY;
    DWORD error=GetKernelObjectSecurity(previous,DACL_SECURITY_INFORMATION,sd,n,&n)?0:GetLastError();
    SECURITY_DESCRIPTOR_CONTROL control;DWORD revision;
    if(!error&&!GetSecurityDescriptorControl(sd,&control,&revision))error=GetLastError();
    if(!error&&!SetKernelObjectSecurity(replacement,DACL_SECURITY_INFORMATION|((control&SE_DACL_PROTECTED)?PROTECTED_DACL_SECURITY_INFORMATION:UNPROTECTED_DACL_SECURITY_INFORMATION),sd))error=GetLastError();
    fpe_free(sd);return error;
}

#endif
