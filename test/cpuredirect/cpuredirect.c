//
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
//

//
// CPU Redirect Test Tool
//
// Creates an XDP program that redirects UDP traffic to specified CPUs for
// software RSS distribution, bypassing hardware RSS queue limitations.
//

#include <xdpapi.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>

CONST CHAR *UsageText =
"cpuredirect.exe <IfIndex> <Port> <CpuBase> <CpuCount> [RingDepth] [DrainBatch] [--azc]\n"
"\n"
"Creates an XDP program that redirects UDP traffic to CPUs for load distribution.\n"
"\n"
"ARGUMENTS:\n"
"\n"
"   IfIndex\n"
"       Network interface index (use 'ipconfig /all' to find)\n"
"\n"
"   Port\n"
"       UDP destination port to match (0 = match all traffic)\n"
"\n"
"   CpuBase\n"
"       First CPU index in the target range\n"
"\n"
"   CpuCount\n"
"       Number of CPUs to distribute across\n"
"\n"
"   RingDepth (optional, default 32768)\n"
"       Per-CPU ring capacity. Must be a power of 2.\n"
"       Only rings for [CpuBase, CpuBase+CpuCount) are allocated.\n"
"\n"
"   DrainBatch (optional, default 256)\n"
"       Max NBLs drained per DPC iteration. Range: 1-256.\n"
"\n"
"   --azc\n"
"       Enable absolute zero-copy mode. Indicates original miniport NBLs\n"
"       directly to TCP/IP without any copy. Requires CanPend=TRUE from miniport.\n"
"\n"
"EXAMPLES:\n"
"\n"
"   cpuredirect.exe 6 9999 40 20\n"
"       Redirect UDP port 9999 traffic across CPUs 40-59 (default ring/batch)\n"
"\n"
"   cpuredirect.exe 6 9999 56 24 16384 128\n"
"       CPUs 56-79, 16K ring depth, 128 drain batch\n"
"\n"
"   cpuredirect.exe 6 0 0 16\n"
"       Redirect ALL traffic across CPUs 0-15\n"
"\n"
"Press Ctrl+C to stop and detach the XDP program.\n"
;

#define LOGERR(...) \
    fprintf(stderr, "ERROR: "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n")

#define LOGINFO(...) \
    printf(__VA_ARGS__); printf("\n")

//
// Event signaled by Ctrl+C handler to wake the main thread for clean shutdown.
//
HANDLE StopEvent = NULL;

BOOL
ParseArgs(
    INT ArgC,
    CHAR **ArgV,
    UINT32 *IfIndex,
    UINT16 *Port,
    UINT32 *CpuBase,
    UINT32 *CpuCount,
    UINT32 *RingDepth,
    UINT32 *DrainBatch,
    UINT32 *Flags
    )
{
    if (ArgC < 5 || ArgC > 8) {
        return FALSE;
    }

    *IfIndex = (UINT32)atoi(ArgV[1]);
    *Port = (UINT16)atoi(ArgV[2]);
    *CpuBase = (UINT32)atoi(ArgV[3]);
    *CpuCount = (UINT32)atoi(ArgV[4]);
    *RingDepth = (ArgC >= 6) ? (UINT32)atoi(ArgV[5]) : 0;
    *DrainBatch = (ArgC >= 7) ? (UINT32)atoi(ArgV[6]) : 0;
    *Flags = 0;

    //
    // Scan for named flags anywhere in the argument list.
    //
    for (INT i = 5; i < ArgC; i++) {
        if (_stricmp(ArgV[i], "--azc") == 0) {
            *Flags |= XDP_CPU_REDIRECT_FLAG_ABSOLUTE_ZERO_COPY;
        }
    }

    //
    // Basic validation.
    //
    if (*IfIndex == 0) {
        LOGERR("Invalid interface index: %s", ArgV[1]);
        return FALSE;
    }

    if (*CpuCount == 0) {
        LOGERR("CPU count must be greater than 0");
        return FALSE;
    }

    if (*RingDepth != 0 && (*RingDepth < 2 || (*RingDepth & (*RingDepth - 1)) != 0)) {
        LOGERR("RingDepth must be 0 (default) or a power of 2 >= 2");
        return FALSE;
    }

    if (*DrainBatch > 256) {
        LOGERR("DrainBatch must be 0 (default) or in range 1-256");
        return FALSE;
    }

    return TRUE;
}

BOOL
CtrlHandler(
    DWORD CtrlType
    )
{
    switch (CtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
        LOGINFO("\nDetaching XDP program...");
        SetEvent(StopEvent);
        return TRUE;  // Signal main thread; don't let default handler call ExitProcess
    default:
        return FALSE;
    }
}

INT
__cdecl
main(
    INT ArgC,
    CHAR **ArgV
    )
{
    UINT32 IfIndex;
    UINT16 Port;
    UINT32 CpuBase;
    UINT32 CpuCount;
    UINT32 RingDepth;
    UINT32 DrainBatch;
    UINT32 Flags;
    XDP_STATUS XdpStatus;
    HANDLE Program = NULL;
    XDP_RULE Rule;
    XDP_CREATE_PROGRAM_FLAGS ProgramFlags;

    const XDP_HOOK_ID XdpInspectRxL2 = {
        XDP_HOOK_L2,
        XDP_HOOK_RX,
        XDP_HOOK_INSPECT,
    };

    //
    // Parse command line arguments.
    //
    if (!ParseArgs(ArgC, ArgV, &IfIndex, &Port, &CpuBase, &CpuCount, &RingDepth, &DrainBatch, &Flags)) {
        printf(UsageText);
        return 1;
    }

    //
    // Display configuration.
    //
    LOGINFO("=== CPU Redirect XDP Program ===");
    LOGINFO("Interface Index: %u", IfIndex);
    if (Port == 0) {
        LOGINFO("Match: ALL traffic");
    } else {
        LOGINFO("Match: UDP destination port %u", Port);
    }
    LOGINFO("Target CPUs: %u-%u (%u cores)",
            CpuBase, CpuBase + CpuCount - 1, CpuCount);
    LOGINFO("Ring Depth:  %u (0=default 32768)", RingDepth);
    LOGINFO("Drain Batch: %u (0=default 256)", DrainBatch);
    LOGINFO("Flags:       0x%x%s", Flags,
            (Flags & XDP_CPU_REDIRECT_FLAG_ABSOLUTE_ZERO_COPY) ? " (absolute zero-copy)" : "");
    LOGINFO("");

    //
    // Build the XDP rule.
    //
    ZeroMemory(&Rule, sizeof(Rule));

    if (Port == 0) {
        Rule.Match = XDP_MATCH_ALL;
    } else {
        Rule.Match = XDP_MATCH_UDP_DST;
        Rule.Pattern.Port = htons(Port);  // Network byte order
    }

    Rule.Action = XDP_PROGRAM_ACTION_REDIRECT;
    Rule.Redirect.TargetType = XDP_REDIRECT_TARGET_TYPE_CPU;
    Rule.Redirect.CpuRedirect.TargetCpuBase = CpuBase;
    Rule.Redirect.CpuRedirect.TargetCpuCount = CpuCount;
    Rule.Redirect.CpuRedirect.RingDepth = RingDepth;
    Rule.Redirect.CpuRedirect.DrainBatchSize = DrainBatch;
    Rule.Redirect.CpuRedirect.Flags = Flags;

    //
    // Use generic mode and apply to all queues.
    //
    ProgramFlags = XDP_CREATE_PROGRAM_FLAG_GENERIC | XDP_CREATE_PROGRAM_FLAG_ALL_QUEUES;

    //
    // Create the XDP program.
    //
    LOGINFO("Creating XDP program...");
    XdpStatus = XdpCreateProgram(
        IfIndex,
        &XdpInspectRxL2,
        0,
        ProgramFlags,
        &Rule,
        1,
        &Program);

    if (FAILED(XdpStatus)) {
        LOGERR("XdpCreateProgram failed: 0x%x", XdpStatus);
        LOGERR("");
        LOGERR("Common issues:");
        LOGERR("  - Invalid interface index (check with 'ipconfig /all')");
        LOGERR("  - XDP driver not installed or not running");
        LOGERR("  - Invalid CPU range (exceeds system CPU count)");
        LOGERR("  - Insufficient permissions (run as Administrator)");
        return 1;
    }

    LOGINFO("XDP program attached successfully!");
    LOGINFO("");
    LOGINFO("The program is now redirecting traffic across %u CPUs.", CpuCount);
    LOGINFO("Press Ctrl+C to detach and exit...");
    LOGINFO("");

    //
    // Set up Ctrl+C handler.
    //
    StopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (StopEvent == NULL) {
        LOGERR("CreateEvent failed: %u", GetLastError());
        CloseHandle(Program);
        return 1;
    }

    SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE);

    //
    // Keep the program running until Ctrl+C signals the stop event.
    //
    WaitForSingleObject(StopEvent, INFINITE);

    //
    // Clean up: close the XDP program handle to trigger kernel-side detach
    // and resource cleanup (including CpuMap destroy + stats dump).
    //
    if (Program != NULL) {
        CloseHandle(Program);
        LOGINFO("XDP program detached.");
    }

    CloseHandle(StopEvent);

    return 0;
}
