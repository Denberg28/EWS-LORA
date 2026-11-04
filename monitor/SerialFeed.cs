using System.IO.Ports;

namespace EwsMonitor;

record ReceivedSnapshot(Snapshot Snapshot, int Connection, DateTimeOffset Received);

sealed class SerialFeed(string port)
{
    private readonly CancellationTokenSource stop = new();
    private ReceivedSnapshot? next;
    private string status = "Opening USB port";
    public string Status => Volatile.Read(ref status);
    public ReceivedSnapshot? Take() => Interlocked.Exchange(ref next, null);
    public void Start() => _ = Task.Run(RunAsync);
    public void Stop() => stop.Cancel();

    private async Task RunAsync()
    {
        char[] buffer = new char[512];
        int connection = 0;
        while (!stop.IsCancellationRequested)
        {
            try
            {
                // USB CDC requires DTR for the Base stream. Keep RTS off and never write.
                using var serial = new SerialPort(port, 115200) { DtrEnable = true, ReadTimeout = 350 };
                serial.Open();
                int epoch = ++connection;
                var reader = new TelemetryReader();
                Volatile.Write(ref status, "USB open · waiting for Base feed");
                while (!stop.IsCancellationRequested)
                {
                    try
                    {
                        int count = serial.Read(buffer, 0, buffer.Length);
                        reader.Feed(buffer.AsSpan(0, count), snapshot =>
                            Interlocked.Exchange(ref next, new(snapshot, epoch, DateTimeOffset.UtcNow)));
                    }
                    catch (TimeoutException)
                    {
                        // Allows prompt shutdown during quiet periods.
                    }
                }
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException)
            {
                Volatile.Write(ref status, "Retrying USB · " +
                    (ex is UnauthorizedAccessException ? "port in use or access denied" : "check Base and cable"));
                Interlocked.Exchange(ref next, null);
            }
            try { await Task.Delay(1500, stop.Token); }
            catch (OperationCanceledException) { return; }
        }
    }
}
