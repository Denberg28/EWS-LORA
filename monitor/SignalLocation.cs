using System.Globalization;

namespace EwsMonitor;

record Anchor(int Id, double Lat, double Lon, double ReferenceRssi, double Exponent, double Margin);
record SignalEvidence(int Age, int[] Rssi);
record SignalEstimate(Location Position, int Radius, int Anchors);

static class SignalLocation
{
    public const string Header = "anchor,lat,lon,rssi_at_100m,path_loss,margin_db";
    private const int Grid = 80;

    public static Anchor[] ReadAnchors(TextReader file)
    {
        if (file.ReadLine()?.Trim() != Header)
            throw new FormatException("Unexpected anchor file header.");
        var result = new Dictionary<int, Anchor>();
        int lineNumber = 1;
        while (file.ReadLine() is string line)
        {
            lineNumber++;
            if (string.IsNullOrWhiteSpace(line))
                continue;
            string[] fields = line.Split(',');
            if (fields.Length != 6 || !int.TryParse(fields[0], NumberStyles.None, CultureInfo.InvariantCulture, out int id) ||
                id is < 0 or > 8 || !Value(fields[1], -85, 85, out double lat) ||
                !Value(fields[2], -180, 180, out double lon) ||
                !Value(fields[3], -130, -25, out double reference) ||
                !Value(fields[4], 1, 6, out double exponent) || !Value(fields[5], 6, 20, out double margin) ||
                !result.TryAdd(id, new(id, lat, lon, reference, exponent, margin)))
                throw new FormatException($"Invalid or duplicate anchor on line {lineNumber}.");
        }
        if (result.Count < 3)
            throw new FormatException("At least three surveyed and calibrated anchors are required.");
        return result.Values.ToArray();
    }

    private static bool Value(string input, double min, double max, out double result) =>
        double.TryParse(input, NumberStyles.Float, CultureInfo.InvariantCulture, out result) &&
        result >= min && result <= max;

    private static double DeltaLon(double lon, double origin) => (lon - origin + 540) % 360 - 180;

    private record struct Reading(double X, double Y, double Rssi, Anchor Anchor);

    public static SignalEstimate? Locate(Anchor[] anchors, SignalEvidence evidence)
    {
        var received = anchors.Where(a => evidence.Rssi[a.Id] != 0).ToArray();
        if (received.Length < 3)
            return null;
        Anchor origin = received[0];
        const double latScale = 111195;
        double lonScale = latScale * Math.Cos(origin.Lat * Math.PI / 180);
        var readings = received.Select(a => new Reading(DeltaLon(a.Lon, origin.Lon) * lonScale,
            (a.Lat - origin.Lat) * latScale, evidence.Rssi[a.Id] / 10.0, a)).ToArray();

        double width = readings.Max(a => a.X) - readings.Min(a => a.X);
        double height = readings.Max(a => a.Y) - readings.Min(a => a.Y);
        double span = Math.Max(width, height);
        if (span is < 20 or > 30000)
            return null;

        var triangles = new List<(Reading A, Reading B, Reading C)>();
        for (int i = 0; i < readings.Length - 2; i++)
            for (int j = i + 1; j < readings.Length - 1; j++)
                for (int k = j + 1; k < readings.Length; k++)
                    if (Math.Abs(Cross(readings[i], readings[j], readings[k].X, readings[k].Y)) >= 0.06 * span * span)
                        triangles.Add((readings[i], readings[j], readings[k]));
        if (triangles.Count == 0)
            return null;

        double step = (2 * span) / Grid;
        double left = readings.Min(a => a.X) + width / 2 - span;
        double bottom = readings.Min(a => a.Y) + height / 2 - span;
        double best = double.PositiveInfinity;
        double xBest = 0, yBest = 0;
        var feasible = new List<(double X, double Y)>();
        for (int j = 0; j <= Grid; j++)
        {
            for (int i = 0; i <= Grid; i++)
            {
                double x = left + i * step, y = bottom + j * step;
                double score = Score(readings, x, y);
                if (!double.IsFinite(score))
                    continue;
                if (i == 0 || j == 0 || i == Grid || j == Grid)
                    return null; // Evidence does not bound the area within the local search.
                feasible.Add((x, y));
                if (score < best)
                    (best, xBest, yBest) = (score, x, y);
            }
        }

        if (feasible.Count == 0)
            return null;
        double coarseX = xBest, coarseY = yBest;
        for (int y = -4; y <= 4; y++)
            for (int x = -4; x <= 4; x++)
            {
                double px = coarseX + x * step / 4, py = coarseY + y * step / 4;
                double score = Score(readings, px, py);
                if (score < best)
                    (best, xBest, yBest) = (score, px, py);
            }

        if (!triangles.Any(t => Inside(t.A, t.B, t.C, xBest, yBest)))
            return null;

        double radius = Math.Sqrt(feasible.Max(p => Squared(p.X - xBest, p.Y - yBest))) + step * Math.Sqrt(2);
        double lon = DeltaLon(origin.Lon + xBest / lonScale, 0);
        double lat = origin.Lat + yBest / latScale;
        return new(new((int)Math.Round(lat * 100000), (int)Math.Round(lon * 100000)),
            (int)Math.Max(25, Math.Ceiling(radius / 25) * 25), readings.Length);
    }

    private static double Score(Reading[] readings, double x, double y)
    {
        double total = 0;
        foreach (Reading r in readings)
        {
            double distance = Math.Max(10, Math.Sqrt(Squared(x - r.X, y - r.Y)));
            double predicted = r.Anchor.ReferenceRssi - 10 * r.Anchor.Exponent * Math.Log10(distance / 100);
            double error = (predicted - r.Rssi) / r.Anchor.Margin;
            if (Math.Abs(error) > 1)
                return double.PositiveInfinity;
            total += error * error;
        }
        return total;
    }

    private static double Squared(double x, double y) => x * x + y * y;
    private static double Cross(Reading a, Reading b, double x, double y) =>
        (b.X - a.X) * (y - a.Y) - (b.Y - a.Y) * (x - a.X);
    private static bool Inside(Reading a, Reading b, Reading c, double x, double y)
    {
        double p = Cross(a, b, x, y), q = Cross(b, c, x, y), r = Cross(c, a, x, y);
        return (p >= 0 && q >= 0 && r >= 0) || (p <= 0 && q <= 0 && r <= 0);
    }
}
