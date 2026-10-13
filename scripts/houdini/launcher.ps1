# [houdini] Job Object launcher (plan decision 8).
#
# hython starts Mantra or husk children of its own. Killing hython alone would
# orphan them, so every Houdini process runs inside a Job Object with
# kill-on-close, and this launcher holds the only handle to it.
#
# Protocol (contain.mjs is the only caller):
#   stdin line 1   base64 of a UTF-8 JSON spec:
#                  {exe, args[], cwd, env["K=V", sorted], stdout, stderr, input}
#   stdout         "LAUNCHED <pid>" once the child runs, then one of
#                  "EXITED <code>"  the child exited; anything left in the job is killed
#                  "KILLED"         stdin closed first; the whole job is killed
#                  "ERROR <text>"   the launch failed
# The child joins the job as it is created (PROC_THREAD_ATTRIBUTE_JOB_LIST), gets
# exactly the environment in the spec, inherits only its three standard handles,
# and reads `input` on stdin; it is resumed only once the input is written. If
# this launcher dies, Windows closes the job handle and kills the tree.
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class HoudiniJob
{
    [StructLayout(LayoutKind.Sequential)]
    private struct BasicLimits
    {
        public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass, SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
        public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ExtendedLimits
    {
        public BasicLimits BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit, JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed, PeakJobMemoryUsed;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Accounting
    {
        public long TotalUserTime, TotalKernelTime, PeriodUserTime, PeriodKernelTime;
        public uint PageFaults, TotalProcesses, ActiveProcesses, TerminatedProcesses;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct SecurityAttributes
    {
        public uint Length;
        public IntPtr Descriptor;
        public int Inherit;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public uint Size;
        public string Reserved, Desktop, Title;
        public uint X, Y, XSize, YSize, XCountChars, YCountChars, FillAttribute, Flags;
        public ushort ShowWindow, ReservedSize;
        public IntPtr ReservedData, Input, Output, Error;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct StartupInfoEx
    {
        public StartupInfo Startup;
        public IntPtr Attributes;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr Process, Thread;
        public uint ProcessId, ThreadId;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(
        IntPtr job, int informationClass, ref ExtendedLimits limits, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool QueryInformationJobObject(IntPtr job, int informationClass,
        out Accounting accounting, uint size, IntPtr returned);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateFileW(string name, uint access, uint share,
        ref SecurityAttributes attributes, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CreatePipe(out IntPtr read, out IntPtr write,
        ref SecurityAttributes attributes, uint size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetHandleInformation(IntPtr handle, uint mask, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(IntPtr handle, byte[] buffer, uint count,
        out uint written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool InitializeProcThreadAttributeList(
        IntPtr list, int count, uint flags, ref IntPtr size);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags,
        IntPtr attribute, IntPtr value, IntPtr size, IntPtr previous, IntPtr returned);
    [DllImport("kernel32.dll")]
    private static extern void DeleteProcThreadAttributeList(IntPtr list);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool CreateProcessW(string application, StringBuilder command,
        IntPtr processAttributes, IntPtr threadAttributes, bool inherit, uint flags,
        IntPtr environment, string directory, ref StartupInfoEx startup,
        out ProcessInformation information);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint code);

    private static IntPtr job = IntPtr.Zero, process = IntPtr.Zero;
    private static volatile bool inputClosed;

    // Console.In's async reads run synchronously in .NET Framework, so EOF on
    // the caller's pipe is watched from a thread of its own.
    public static void WatchInput()
    {
        Thread watcher = new Thread(() =>
        {
            try
            {
                Stream stream = Console.OpenStandardInput();
                byte[] buffer = new byte[256];
                while (stream.Read(buffer, 0, buffer.Length) > 0) { }
            }
            catch (IOException) { }
            inputClosed = true;
        });
        watcher.IsBackground = true;
        watcher.Start();
    }

    public static bool InputClosed { get { return inputClosed; } }

    private static string Quote(string value)
    {
        if (value.Length > 0 && value.IndexOfAny(new char[] { ' ', '\t', '"' }) < 0)
            return value;
        StringBuilder quoted = new StringBuilder("\"");
        int slashes = 0;
        foreach (char character in value)
        {
            if (character == '\\') { slashes++; continue; }
            quoted.Append('\\', character == '"' ? slashes * 2 + 1 : slashes);
            quoted.Append(character);
            slashes = 0;
        }
        quoted.Append('\\', slashes * 2);
        quoted.Append('"');
        return quoted.ToString();
    }

    private static IntPtr OpenOutput(string path, ref SecurityAttributes inheritable)
    {
        // GENERIC_WRITE, shared read/write, CREATE_ALWAYS.
        IntPtr handle = CreateFileW(path, 0x40000000, 3, ref inheritable, 2, 0x80, IntPtr.Zero);
        if (handle == new IntPtr(-1)) throw new Win32Exception();
        return handle;
    }

    public static uint Start(string exe, string[] args, string cwd, string[] env,
        string stdoutPath, string stderrPath, byte[] input)
    {
        job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero) throw new Win32Exception();
        ExtendedLimits limits = new ExtendedLimits();
        // KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION (no error dialog can hold a child).
        limits.BasicLimitInformation.LimitFlags = 0x2000 | 0x400;
        if (!SetInformationJobObject(job, 9, ref limits, (uint)Marshal.SizeOf(typeof(ExtendedLimits))))
            throw new Win32Exception();

        SecurityAttributes inheritable = new SecurityAttributes();
        inheritable.Length = (uint)Marshal.SizeOf(typeof(SecurityAttributes));
        inheritable.Inherit = 1;
        IntPtr inputRead = IntPtr.Zero, inputWrite = IntPtr.Zero, output = IntPtr.Zero, error = IntPtr.Zero;
        IntPtr attributes = IntPtr.Zero, handles = IntPtr.Zero, jobs = IntPtr.Zero, block = IntPtr.Zero;
        bool initialized = false;
        ProcessInformation information = new ProcessInformation();
        try
        {
            // The pipe holds the whole job, so writing it never waits on the child.
            if (!CreatePipe(out inputRead, out inputWrite, ref inheritable, (uint)Math.Max(65536, input.Length + 4096)))
                throw new Win32Exception();
            if (!SetHandleInformation(inputWrite, 1, 0)) throw new Win32Exception();
            output = OpenOutput(stdoutPath, ref inheritable);
            error = OpenOutput(stderrPath, ref inheritable);

            IntPtr size = IntPtr.Zero;
            InitializeProcThreadAttributeList(IntPtr.Zero, 2, 0, ref size);
            attributes = Marshal.AllocHGlobal(size);
            if (!InitializeProcThreadAttributeList(attributes, 2, 0, ref size)) throw new Win32Exception();
            initialized = true;
            handles = Marshal.AllocHGlobal(IntPtr.Size * 3);
            Marshal.WriteIntPtr(handles, 0, inputRead);
            Marshal.WriteIntPtr(handles, IntPtr.Size, output);
            Marshal.WriteIntPtr(handles, IntPtr.Size * 2, error);
            // PROC_THREAD_ATTRIBUTE_HANDLE_LIST: inherit the three standard handles only.
            if (!UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x20002), handles,
                new IntPtr(IntPtr.Size * 3), IntPtr.Zero, IntPtr.Zero)) throw new Win32Exception();
            jobs = Marshal.AllocHGlobal(IntPtr.Size);
            Marshal.WriteIntPtr(jobs, job);
            // PROC_THREAD_ATTRIBUTE_JOB_LIST: in the job from creation, before any code runs.
            if (!UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x2000D), jobs,
                new IntPtr(IntPtr.Size), IntPtr.Zero, IntPtr.Zero)) throw new Win32Exception();

            StartupInfoEx startup = new StartupInfoEx();
            startup.Startup.Size = (uint)Marshal.SizeOf(typeof(StartupInfoEx));
            startup.Startup.Flags = 0x100; // STARTF_USESTDHANDLES
            startup.Startup.Input = inputRead;
            startup.Startup.Output = output;
            startup.Startup.Error = error;
            startup.Attributes = attributes;
            StringBuilder command = new StringBuilder("\"").Append(exe).Append("\"");
            foreach (string argument in args) command.Append(" ").Append(Quote(argument));
            block = Marshal.StringToHGlobalUni(string.Join("\0", env) + "\0\0");
            // CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW
            if (!CreateProcessW(exe, command, IntPtr.Zero, IntPtr.Zero, true, 0x08080404,
                block, cwd, ref startup, out information)) throw new Win32Exception();
            process = information.Process;

            uint written;
            if (input.Length > 0 && (!WriteFile(inputWrite, input, (uint)input.Length, out written, IntPtr.Zero) ||
                written != (uint)input.Length)) throw new Win32Exception();
            CloseHandle(inputWrite);
            inputWrite = IntPtr.Zero;
            if (ResumeThread(information.Thread) == UInt32.MaxValue) throw new Win32Exception();
            return information.ProcessId;
        }
        finally
        {
            if (information.Thread != IntPtr.Zero) CloseHandle(information.Thread);
            if (initialized) DeleteProcThreadAttributeList(attributes);
            if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
            if (handles != IntPtr.Zero) Marshal.FreeHGlobal(handles);
            if (jobs != IntPtr.Zero) Marshal.FreeHGlobal(jobs);
            if (block != IntPtr.Zero) Marshal.FreeHGlobal(block);
            if (inputRead != IntPtr.Zero) CloseHandle(inputRead);
            if (inputWrite != IntPtr.Zero) CloseHandle(inputWrite);
            if (output != IntPtr.Zero) CloseHandle(output);
            if (error != IntPtr.Zero) CloseHandle(error);
        }
    }

    public static bool Exited(int milliseconds)
    {
        uint result = WaitForSingleObject(process, (uint)milliseconds);
        if (result == UInt32.MaxValue) throw new Win32Exception();
        return result == 0;
    }

    public static int ExitCode()
    {
        uint code;
        if (!GetExitCodeProcess(process, out code)) throw new Win32Exception();
        return unchecked((int)code);
    }

    // Kill everything still in the job and wait until it is gone, so the
    // caller never sees a result while a child still holds files open.
    public static bool KillAll(int milliseconds)
    {
        if (job == IntPtr.Zero) return true;
        if (!TerminateJobObject(job, 1)) throw new Win32Exception();
        Stopwatch timer = Stopwatch.StartNew();
        while (true)
        {
            Accounting accounting;
            if (!QueryInformationJobObject(job, 1, out accounting,
                (uint)Marshal.SizeOf(typeof(Accounting)), IntPtr.Zero)) throw new Win32Exception();
            if (accounting.ActiveProcesses == 0) return true;
            if (timer.ElapsedMilliseconds >= milliseconds) return false;
            Thread.Sleep(20);
        }
    }
}
'@

function Write-Line([string]$Text) {
    [Console]::Out.WriteLine($Text)
    [Console]::Out.Flush()
}

try {
    $line = [Console]::In.ReadLine()
    if (-not $line) { throw 'no launch spec on stdin' }
    $spec = [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($line)) | ConvertFrom-Json
    $stdinBytes = [Text.Encoding]::UTF8.GetBytes([string]$spec.input)
    $processId = [HoudiniJob]::Start([string]$spec.exe, [string[]]@($spec.args), [string]$spec.cwd,
        [string[]]@($spec.env), [string]$spec.stdout, [string]$spec.stderr, $stdinBytes)
} catch {
    Write-Line ('ERROR ' + ($_.Exception.Message -replace '[\r\n]+', ' '))
    exit 1
}
Write-Line "LAUNCHED $processId"

# Closing stdin is the caller's kill switch.
[HoudiniJob]::WatchInput()
while ($true) {
    if ([HoudiniJob]::Exited(100)) {
        $code = [HoudiniJob]::ExitCode()
        if (-not [HoudiniJob]::KillAll(10000)) { Write-Line 'ERROR the job did not drain'; exit 1 }
        Write-Line "EXITED $code"
        exit 0
    }
    if ([HoudiniJob]::InputClosed) {
        $drained = [HoudiniJob]::KillAll(10000)
        Write-Line ($(if ($drained) { 'KILLED' } else { 'ERROR the job did not drain' }))
        exit $(if ($drained) { 0 } else { 1 })
    }
}
