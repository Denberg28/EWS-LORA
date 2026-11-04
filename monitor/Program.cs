namespace EwsMonitor;

internal static class Program
{
    [STAThread]
    private static int Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        using var form = new MonitorForm();
        if (args.Length == 1 && args[0] == "--smoke")
        {
            form.Show();
            return form.SmokeTest() ? 0 : 1;
        }
        Application.Run(form);
        return 0;
    }
}
