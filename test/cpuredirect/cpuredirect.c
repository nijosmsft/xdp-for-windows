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
"cpuredirect.exe <IfIndex> <Port> <CpuBase> <CpuCount>\n"
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
"       First CPU index in the target range (typically 0)\n"
"\n"
"   CpuCount\n"
"       Number of CPUs to distribute across\n"
"\n"
"EXAMPLES:\n"
"\n"
"   cpuredirect.exe 6 9999 0 80\n"
"       Redirect UDP port 9999 traffic across CPUs 0-79\n"
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
    UINT32 *CpuCount
    )
{
    if (ArgC != 5) {
        return FALSE;
    }

    *IfIndex = (UINT32)atoi(ArgV[1]);
    *Port = (UINT16)atoi(ArgV[2]);
    *CpuBase = (UINT32)atoi(ArgV[3]);
    *CpuCount = (UINT32)atoi(ArgV[4]);

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
    if (!ParseArgs(ArgC, ArgV, &IfIndex, &Port, &CpuBase, &CpuCount)) {
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
