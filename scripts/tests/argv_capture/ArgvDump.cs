using System;
using System.Diagnostics;
using System.IO;
using System.Text;

// rsync argv recorder.
//
// Put this exe next to SyncReplayTool.exe (as "rsync.exe") so the product's
// own launcher picks it up, then it records the argv it was called with and
// forwards the very same argv to the real rsync.exe.
//
// Why a wrapper at all: the product starts rsync by bare name, so the only
// place the true command line can be observed from the outside is the process
// itself. Reading the source only proves what the code *intends* to run;
// this proves what was *actually* executed.
//
// Env:
//   SRT_ARGV_DUMP    file to append the argv dump to (UTF-8)
//   SRT_REAL_RSYNC   absolute path of the real rsync.exe to forward to
static class ArgvDump
{
    // Windows command line quoting (same rules CreateProcess parses).
    static string Quote(string s)
    {
        if (s.Length > 0 && s.IndexOfAny(new char[] { ' ', '\t', '"', '\n', '\v' }) < 0)
            return s;
        StringBuilder sb = new StringBuilder();
        sb.Append('"');
        int slashes = 0;
        foreach (char c in s)
        {
            if (c == '\\') { slashes++; continue; }
            if (c == '"')
            {
                sb.Append('\\', slashes * 2 + 1);
                sb.Append('"');
                slashes = 0;
                continue;
            }
            sb.Append('\\', slashes);
            slashes = 0;
            sb.Append(c);
        }
        sb.Append('\\', slashes * 2);
        sb.Append('"');
        return sb.ToString();
    }

    static int Main(string[] args)
    {
        string dump = Environment.GetEnvironmentVariable("SRT_ARGV_DUMP");
        string real = Environment.GetEnvironmentVariable("SRT_REAL_RSYNC");
        string cwd = "";
        try { cwd = Directory.GetCurrentDirectory(); } catch (Exception) { }

        try
        {
            if (!string.IsNullOrEmpty(dump))
            {
                StringBuilder sb = new StringBuilder();
                sb.AppendLine("=== INVOCATION ===");
                sb.AppendLine("ARGV_COUNT=" + args.Length);
                for (int i = 0; i < args.Length; i++)
                    sb.AppendLine("[" + i + "] " + args[i]);
                sb.AppendLine("CWD=" + cwd);
                // 工作目录里的文件清单：用来证明「--files-from 指的那个清单文件
                // 确实就在 rsync 的工作目录里」（同步结束后产品会把它删掉，
                // 所以只能在这里看）。
                try
                {
                    foreach (string f in Directory.GetFiles(cwd))
                        sb.AppendLine("CWD_FILE=" + Path.GetFileName(f));
                }
                catch (Exception) { }
                File.AppendAllText(dump, sb.ToString(), new UTF8Encoding(false));
            }
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("argv-dump: " + e.Message);
        }

        if (string.IsNullOrEmpty(real))
            return 0;

        ProcessStartInfo psi = new ProcessStartInfo(real);
        psi.UseShellExecute = false;
        StringBuilder cl = new StringBuilder();
        foreach (string a in args)
        {
            cl.Append(Quote(a));
            cl.Append(' ');
        }
        psi.Arguments = cl.ToString().TrimEnd();
        try
        {
            using (Process p = Process.Start(psi))
            {
                p.WaitForExit();
                return p.ExitCode;
            }
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("argv-dump: exec failed: " + e.Message);
            return 127;
        }
    }
}
