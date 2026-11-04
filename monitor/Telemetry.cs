using System.Globalization;
using System.Text;

namespace EwsMonitor;

enum BaseState { READY, CONFIG, RADIO, STORE }
enum Alarm { None, Sos, Cleared }
enum Fix { Unknown, Missing, Valid }
record struct Location(int Lat, int Lon)
{
    public override string ToString() => FormattableString.Invariant($"{Lat / 1e5:F5}, {Lon / 1e5:F5}");
}

record Rover(int Id, int Route, string Name, Alarm Alarm, bool Restored, int ContactAge,
    Fix Fix, int FixAge, Location Position, uint Point, uint Segment, bool Cached)
{
    public string Label => Name.Length == 0 ? $"Rover {Id}" : Name;
    public bool Seen => ContactAge >= 0 || Alarm != Alarm.None;
    public bool HasPosition => Point > 0 || Cached;
}

record Node(int Id, int HeartbeatAge, int ForwardAge);
record Snapshot(ulong Base, ulong Boot, long Uptime, BaseState State, Rover[] Rovers, Node[] Nodes);

sealed class TelemetryReader
{
    private readonly StringBuilder line = new(128);
    private bool overflow;
    private Snapshot? pending;
    private int row;

    public void Feed(ReadOnlySpan<char> data, Action<Snapshot> onSnapshot)
    {
        foreach (char ch in data)
        {
            if (ch == '\n')
            {
                if (overflow)
                    pending = null;
                else if (Parse(line.ToString().TrimEnd('\r')) is Snapshot snapshot)
                    onSnapshot(snapshot);
                line.Clear();
                overflow = false;
            }
            else if (!overflow)
            {
                if (line.Length == 128)
                    overflow = true;
                else
                    line.Append(ch);
            }
        }
    }

    private Snapshot? Parse(string value)
    {
        if (!value.StartsWith("EWS1,", StringComparison.Ordinal))
            return null; // Base diagnostic logs can appear between complete rows.

        string[] f = value.Split(',');
        if (f.Length == 6 && f[1] == "B" && ulong.TryParse(f[2], out ulong board) && board > 0 &&
            ulong.TryParse(f[3], out ulong boot) && boot > 0 && long.TryParse(f[4], out long up) &&
            up >= 0 && Enum.TryParse(f[5], out BaseState state) && state.ToString() == f[5])
        {
            pending = new(board, boot, up, state, new Rover[8], new Node[8]);
            row = 0;
            return null;
        }

        Snapshot? current = pending;
        if (current == null)
            return null;
        if (row < 8 && f.Length == 15 && f[1] == "R" && Number(f[2], 1, 8, out int id) &&
            id == row + 1 && Number(f[3], 0, 8, out int route) && f[4].Length <= 8 &&
            f[4].All(ch => ch is >= 'A' and <= 'Z' or >= '0' and <= '9' or '-') &&
            Number(f[5], 0, 2, out int alarm) && Number(f[6], 0, 1, out int restored) &&
            Age(f[7], out int contact) && Number(f[8], 0, 2, out int fix) && Age(f[9], out int fixAge) &&
            Number(f[10], -9000000, 9000000, out int lat) && Number(f[11], -18000000, 18000000, out int lon) &&
            uint.TryParse(f[12], out uint point) && uint.TryParse(f[13], out uint segment) &&
            Number(f[14], 0, 1, out int cached) && (point == 0) == (fixAge < 0) &&
            (point == 0) == (segment == 0) && (fix != (int)Fix.Valid || point > 0) &&
            (cached == 0 || point == 0))
        {
            current.Rovers[row++] = new(id, route, f[4], (Alarm)alarm, restored != 0, contact,
                (Fix)fix, fixAge, new(lat, lon), point, segment, cached != 0);
            return null;
        }

        if (row is >= 8 and < 16 && f.Length == 5 && f[1] == "N" &&
            Number(f[2], 1, 8, out int node) && node == row - 7 &&
            Age(f[3], out int beat) && Age(f[4], out int forward))
        {
            current.Nodes[row++ - 8] = new(node, beat, forward);
            return null;
        }

        pending = null;
        return row == 16 && value == "EWS1,E" ? current : null;
    }

    private static bool Age(string input, out int number) => Number(input, -1, int.MaxValue, out number);
    private static bool Number(string input, int min, int max, out int number) =>
        int.TryParse(input, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out number) &&
        number >= min && number <= max;
}

record TrackPoint(Location Position, DateTimeOffset BaseReceipt, int Leg);

sealed class Fleet
{
    public const int RoverLate = 170;
    public const int NodeLate = 190;
    private const int MaxTrack = 2048;
    public Snapshot? Latest { get; private set; }
    public List<TrackPoint>[] Tracks { get; } = Enumerable.Range(0, 8).Select(_ => new List<TrackPoint>()).ToArray();
    private readonly uint[] lastPoint = new uint[8];
    private readonly uint[] lastSegment = new uint[8];
    private readonly int[] legs = new int[8];
    private bool split;

    public void Interrupt() => split = true;

    public void Reset()
    {
        Latest = null;
        split = false;
        Array.Clear(lastPoint);
        Array.Clear(lastSegment);
        Array.Clear(legs);
        foreach (var track in Tracks)
            track.Clear();
    }

    public void Apply(Snapshot snapshot, DateTimeOffset received)
    {
        if (Latest is Snapshot last)
        {
            if (last.Base != snapshot.Base)
                Reset();
            else if (last.Boot != snapshot.Boot)
            {
                Interrupt();
                Array.Clear(lastPoint);
                Array.Clear(lastSegment);
            }
            else if (snapshot.Uptime < last.Uptime)
                return;
        }

        if (split)
            for (int i = 0; i < legs.Length; i++)
                legs[i]++;
        split = false;
        Latest = snapshot;
        if (snapshot.State != BaseState.READY)
        {
            Interrupt();
            return;
        }
        foreach (var rover in snapshot.Rovers)
        {
            int i = rover.Id - 1;
            if (rover.Point == 0 || rover.Point <= lastPoint[i] || rover.Fix != Fix.Valid ||
                rover.FixAge >= RoverLate)
                continue;

            var track = Tracks[i];
            if (lastSegment[i] != rover.Segment)
                legs[i]++;
            lastPoint[i] = rover.Point;
            lastSegment[i] = rover.Segment;
            if (track.Count > 0 && track[^1].Leg == legs[i] && track[^1].Position == rover.Position)
                continue;
            if (track.Count == MaxTrack)
                track.RemoveAt(0);
            track.Add(new(rover.Position, received.AddSeconds(-rover.FixAge), legs[i]));
        }
    }
}
