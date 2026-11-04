using EwsMonitor;

static void Require(bool condition, string message)
{
    if (!condition)
        throw new InvalidOperationException(message);
}

static string Frame(string first, int time = 2, int boot = 11, int board = 22, string state = "READY")
{
    var lines = new List<string> { $"EWS1,B,{board},{boot},{time},{state}", first, "RADIO READY" };
    for (int i = 2; i <= 8; i++)
        lines.Add($"EWS1,R,{i},0,,0,0,-1,0,-1,0,0,0,0,0");
    for (int i = 1; i <= 8; i++)
        lines.Add($"EWS1,N,{i},-1,{(i == 1 ? 0 : -1)}");
    lines.Add("EWS1,E");
    return string.Join("\r\n", lines) + "\r\n";
}

const string restored = "EWS1,R,1,1,ALPHA,1,1,-1,0,-1,0,0,0,0,1";
const string position = "EWS1,R,1,1,ALPHA,1,0,0,2,0,0,0,1,1,0";
const string movement = "EWS1,R,1,1,ALPHA,1,0,0,2,0,3,-7,2,1,0";
const string lostFix = "EWS1,R,1,1,ALPHA,1,0,0,1,2,3,-7,2,1,0";
const string recovered = "EWS1,R,1,1,ALPHA,1,0,0,2,0,4,-8,3,2,0";
const string afterDisconnect = "EWS1,R,1,1,ALPHA,1,0,0,2,0,5,-8,4,2,0";
const string overdue = "EWS1,R,1,1,ALPHA,1,0,500,2,500,6,-9,5,2,0";
const string cleared = "EWS1,R,1,1,ALPHA,2,0,0,0,2,5,-8,4,2,0";

var reader = new TelemetryReader();
var fleet = new Fleet();
int frames = 0;
void Feed(string input)
{
    foreach (var chunk in input.Chunk(7))
        reader.Feed(chunk, snapshot =>
        {
            frames++;
            fleet.Apply(snapshot, DateTimeOffset.UnixEpoch.AddSeconds(snapshot.Uptime));
        });
}

Feed(Frame(restored));
Require(fleet.Latest!.Rovers[0].HasPosition && fleet.Latest.Rovers[0].Restored &&
        fleet.Tracks[0].Count == 0, "A restored (0,0) fix should be a marker only.");
Feed(Frame(position, 4).Replace("EWS1,N,5", "EWS1,N,4"));
Feed(Frame(position, 4).Replace(position, position + new string('1', 150)));
Feed(Frame(position, 4).Replace("EWS1,E", ""));
Require(frames == 1, "Malformed or incomplete frames must be rejected.");
Feed(Frame(position, 4));
Feed(Frame(position, 6));
Feed(Frame(movement, 8));
Require(fleet.Tracks[0].Count == 2 && fleet.Tracks[0][0].Leg == fleet.Tracks[0][1].Leg,
    "Movement joins the previous fix, repeated snapshots add no duplicate points.");
Require(fleet.Latest!.Nodes[0] == new Node(1, -1, 0), "Forwarding is independent of heartbeat.");
Feed(Frame(lostFix, 10));
Feed(Frame(recovered, 12));
Require(fleet.Tracks[0].Count == 3 && fleet.Tracks[0][2].Leg != fleet.Tracks[0][1].Leg,
    "Fix loss breaks a track leg.");
fleet.Interrupt();
Feed(Frame(afterDisconnect, 14));
Require(fleet.Tracks[0].Count == 4 && fleet.Tracks[0][3].Leg != fleet.Tracks[0][2].Leg,
    "USB interruption breaks a track leg.");
Feed(Frame(overdue, 514));
Require(fleet.Tracks[0].Count == 4 && fleet.Latest!.Rovers[0].Alarm == Alarm.Sos,
    "Stale data must not add a new point or clear a saved SOS.");
Feed(Frame(cleared, 515));
Require(fleet.Tracks[0].Count == 4 && fleet.Latest!.Rovers[0].Alarm == Alarm.Cleared,
    "CLEAR changes incident state without using its cached coordinate as a new fix.");
Feed(Frame(position, 3, boot: 12));
Require(fleet.Tracks[0].Count == 5 && fleet.Tracks[0][4].Leg != fleet.Tracks[0][3].Leg,
    "Base reboot resets sample counters and splits the track.");
Feed(Frame(position, 3, board: 23));
Require(fleet.Tracks[0].Count == 1, "Changing to another Base isolates session tracks.");
Console.WriteLine("Monitor stream and tracking checks passed.");

using var anchorInput = new StringReader(SignalLocation.Header + "\n" +
    "0,12.5,123.5,-80,3,6\n1,12.5,123.502,-80,3,6\n2,12.502,123.501,-80,3,6");
Anchor[] anchors = SignalLocation.ReadAnchors(anchorInput);
var samples = new int[9];
foreach (Anchor a in anchors)
{
    double x = (a.Lon - 123.501) * 111195 * Math.Cos(12.5 * Math.PI / 180);
    double y = (a.Lat - 12.50065) * 111195;
    samples[a.Id] = (int)Math.Round(10 * (a.ReferenceRssi - 10 * a.Exponent *
        Math.Log10(Math.Sqrt(x * x + y * y) / 100)));
}
var evidence = new SignalEvidence(12, samples);
SignalEstimate? estimate = SignalLocation.Locate(anchors, evidence);
Require(estimate != null && Math.Abs(estimate.Position.Lat - 1250065) <= 4 &&
        Math.Abs(estimate.Position.Lon - 12350100) <= 4 && estimate.Radius >= 25,
    "Calibrated synthetic readings should locate the target and return an area.");
Require(SignalLocation.Locate(anchors.Take(2).ToArray(), evidence) == null,
    "Two anchors must not produce an estimate.");
Require(SignalLocation.Locate(anchors.Select(a => a with { Lat = 12.5 }).ToArray(), evidence) == null,
    "Collinear anchors must not produce an estimate.");
Require(SignalLocation.Locate(anchors, new(12, [-1400, -1400, -1400, 0, 0, 0, 0, 0, 0])) == null,
    "Incompatible readings must not produce an estimate.");
string raw = $"EWSL,A,1,12,{string.Join(",", samples)}\r\n";
Feed(Frame(position, 5, board: 23).Replace("EWS1,E", raw + "EWS1,E"));
Require(fleet.Latest!.Signals[0]?.Rssi.SequenceEqual(samples) == true,
    "The USB snapshot must carry all readings as a separate observation.");
Feed(Frame(position, 6, board: 23).Replace("EWS1,E", raw.Replace(",12,", ",75,") + "EWS1,E"));
Require(fleet.Latest!.Signals[0] == null, "Expired evidence must not survive a new snapshot.");
Console.WriteLine("RF signal location checks passed.");
