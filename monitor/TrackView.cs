using System.Drawing.Drawing2D;

namespace EwsMonitor;

sealed class TrackView : Control
{
    public static readonly Color Ink = Color.FromArgb(226, 237, 245);
    public static readonly Color Muted = Color.FromArgb(150, 170, 189);
    public static readonly Color Canvas = Color.FromArgb(13, 24, 36);
    public static readonly Color[] Colors = [
        Color.DeepSkyBlue, Color.MediumSpringGreen, Color.Orange, Color.Plum,
        Color.Gold, Color.Turquoise, Color.CornflowerBlue, Color.LightSalmon
    ];
    private readonly Fleet fleet;
    private int selected;
    private bool connected;
    private float zoom = 1;

    public TrackView(Fleet fleet)
    {
        this.fleet = fleet;
        DoubleBuffered = true;
        ResizeRedraw = true;
    }

    public void ShowFleet(int id, bool live)
    {
        if (selected != id)
            zoom = 1;
        selected = id;
        connected = live;
        Invalidate();
    }

    public void Zoom(float factor)
    {
        zoom = factor == 0 ? 1 : Math.Clamp(zoom * factor, 0.25f, 16f);
        Invalidate();
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.Clear(Canvas);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        using var text = new SolidBrush(Ink);
        using var muted = new SolidBrush(Muted);
        g.DrawString("OFFLINE TRACK PLOT", Font, text, 18, 15);
        g.DrawString("N ↑", Font, muted, Math.Max(18, Width - 55), 15);
        if (Width < 130 || Height < 130)
            return;
        Rover[] visible = fleet.Latest?.Rovers.Where(r => selected == 0 || r.Id == selected).ToArray() ?? [];
        var locations = visible.SelectMany(r => fleet.Tracks[r.Id - 1].Select(p => p.Position)
            .Concat(r.HasPosition ? [r.Position] : Array.Empty<Location>())).ToArray();
        if (locations.Length == 0)
        {
            g.DrawString("Waiting for a rover position", Font, muted, 18, Height / 2f);
            return;
        }

        Location origin = locations[0];
        double lonScale = Math.Max(0.00001, Math.Cos(origin.Lat / 1e5 * Math.PI / 180));
        (double X, double Y) Project(Location p)
        {
            long delta = (long)p.Lon - origin.Lon;
            double lon = ((delta + 54000000) % 36000000 - 18000000) * lonScale;
            return (lon, origin.Lat - p.Lat);
        }

        var xy = locations.Select(Project).ToArray();
        double minX = xy.Min(p => p.X), maxX = xy.Max(p => p.X);
        double minY = xy.Min(p => p.Y), maxY = xy.Max(p => p.Y);
        var plot = new Rectangle(28, 45, Width - 56, Height - 98);
        double scale = zoom * Math.Min(plot.Width / Math.Max(60, maxX - minX),
            plot.Height / Math.Max(60, maxY - minY));
        double centerX = (maxX + minX) / 2, centerY = (maxY + minY) / 2;
        PointF Screen(Location p)
        {
            var q = Project(p);
            return new(plot.Left + plot.Width / 2f + (float)((q.X - centerX) * scale),
                plot.Top + plot.Height / 2f + (float)((q.Y - centerY) * scale));
        }

        var saved = g.Save();
        g.SetClip(plot);
        using (var grid = new Pen(Color.FromArgb(35, 53, 71)))
        {
            for (int x = plot.Left; x <= plot.Right; x += 48)
                g.DrawLine(grid, x, plot.Top, x, plot.Bottom);
            for (int y = plot.Top; y <= plot.Bottom; y += 48)
                g.DrawLine(grid, plot.Left, y, plot.Right, y);
        }

        foreach (var rover in visible)
        {
            using var trail = new Pen(Colors[rover.Id - 1], 2.5f) { LineJoin = LineJoin.Round };
            foreach (var leg in fleet.Tracks[rover.Id - 1].GroupBy(p => p.Leg))
            {
                var points = leg.Select(p => Screen(p.Position)).ToArray();
                if (points.Length > 1)
                    g.DrawLines(trail, points);
                else
                    g.DrawEllipse(trail, points[0].X - 2, points[0].Y - 2, 4, 4);
            }

            if (!rover.HasPosition)
                continue;
            PointF point = Screen(rover.Position);
            Color color = rover.Alarm == Alarm.Sos ? Color.Tomato : Colors[rover.Id - 1];
            using var marker = new SolidBrush(color);
            using var outline = new Pen(color, 2);
            if (connected && rover.Fix == Fix.Valid && rover.FixAge < Fleet.RoverLate)
                g.FillEllipse(marker, point.X - 6, point.Y - 6, 12, 12);
            else
                g.DrawEllipse(outline, point.X - 6, point.Y - 6, 12, 12);
            g.DrawString($"{rover.Label}{(rover.Alarm == Alarm.Sos ? " · SOS" : "")}", Font, text,
                point.X + 9, point.Y - Font.Height / 2f);
        }

        g.Restore(saved);
        g.DrawString("Solid marker · recent fix     Open marker · last known", Font, muted, 18, Height - 43);
        double meters = 60 / scale * 1.1132;
        string distance = meters >= 1000 ? $"{meters / 1000:0.#} km" : $"{meters:0.#} m";
        using var scaleLine = new Pen(Muted, 2);
        g.DrawLine(scaleLine, 19, Height - 18, 79, Height - 18);
        g.DrawString($"≈ {distance}", Font, muted, 86, Height - 27);
    }
}
