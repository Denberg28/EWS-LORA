using System.Diagnostics;
using System.IO.Ports;
using System.Text;
using System.Text.Json;

namespace EwsMonitor;

sealed class MonitorForm : Form
{
    private readonly Fleet fleet = new();
    private readonly ComboBox ports = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 96 };
    private readonly Button connect = new() { Text = "Connect", AutoSize = true };
    private readonly Button demo = new() { Text = "Demo", AutoSize = true };
    private readonly Label connection = TextLabel("Connect the Base by USB to begin.", 11);
    private readonly Label alert = TextLabel("WAITING FOR BASE", 17);
    private readonly Label details = TextLabel("Select a rover to inspect its last known position.", 10);
    private readonly ListView rovers = Table(("Rover", 98), ("Incident", 105), ("Contact", 88), ("Route", 78), ("GPS", 94));
    private readonly ListView nodes = Table(("Relay", 95), ("Status", 121), ("Heartbeat", 105), ("Forwarded", 104));
    private readonly Button copy = new() { Text = "Copy GPS", AutoSize = true, Enabled = false };
    private readonly Button export = new() { Text = "Export tracks", AutoSize = true, Enabled = false };
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 400 };
    private readonly TrackView plot;
    private static readonly Anchor[] DemoAnchors = [
        new(0, 12.5, 123.5, -80, 3, 6), new(1, 12.5, 123.502, -80, 3, 6),
        new(2, 12.502, 123.501, -80, 3, 6)
    ];
    private Anchor[] anchors = [];
    private ulong configuredBase;
    private Snapshot? evaluatedSnapshot;
    private readonly SignalEstimate?[] estimates = new SignalEstimate?[8];
    private readonly Button anchorSetup = new() { Text = "Load anchors", AutoSize = true };
    private SerialFeed? feed;
    private bool demoActive;
    private bool linkUp;
    private bool updating;
    private int demoStep;
    private long lastRead;
    private int connectionEpoch;
    private long demoAt;
    private string? activePort;
    private string? lastPort;

    public MonitorForm()
    {
        Text = "EWS LoRa · Fleet Monitor";
        StartPosition = FormStartPosition.CenterScreen;
        AutoScaleMode = AutoScaleMode.Dpi;
        MinimumSize = new(950, 680);
        ClientSize = new(1260, 770);
        Font = new("Segoe UI", 10);
        BackColor = Color.FromArgb(19, 33, 48);
        ForeColor = TrackView.Ink;
        plot = new(fleet) { Dock = DockStyle.Fill, Font = Font };
        var layout = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new(16), ColumnCount = 1, RowCount = 3 };
        layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.AutoSize));
        layout.RowStyles.Add(new(SizeType.Percent, 100));
        var heading = Flow(TextLabel("EWS LoRa    /    FLEET MONITOR", 17), ports, Action("Refresh", RefreshPorts),
            connect, demo, anchorSetup);
        layout.Controls.Add(heading, 0, 0);
        layout.Controls.Add(connection, 0, 1);

        var body = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 1 };
        body.ColumnStyles.Add(new(SizeType.Absolute, 500));
        body.ColumnStyles.Add(new(SizeType.Percent, 100));
        var sidebar = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 1,
            RowCount = 8, Margin = new(0, 0, 12, 0) };
        for (int i = 0; i < 7; i++)
            sidebar.RowStyles.Add(new(SizeType.AutoSize));
        sidebar.RowStyles.Add(new(SizeType.Percent, 100));
        sidebar.Controls.Add(alert, 0, 0);
        sidebar.Controls.Add(TextLabel("ROVERS", 10), 0, 1);
        sidebar.Controls.Add(rovers, 0, 2);
        sidebar.Controls.Add(details, 0, 3);
        sidebar.Controls.Add(Flow(Action("Fleet view", () => {
            if (rovers.SelectedItems.Count > 0) rovers.SelectedItems[0].Selected = false;
        }), copy, export), 0, 4);
        sidebar.Controls.Add(TextLabel("RELAY NODES", 10), 0, 5);
        sidebar.Controls.Add(nodes, 0, 6);
        sidebar.Controls.Add(TextLabel("Contact ages are measured at the Base.", 9), 0, 7);
        body.Controls.Add(sidebar, 0, 0);

        var view = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 2 };
        view.RowStyles.Add(new(SizeType.AutoSize));
        view.RowStyles.Add(new(SizeType.Percent, 100));
        view.Controls.Add(Flow(TextLabel("MOVEMENT", 10), Action("Fit", () => plot.Zoom(0)),
            Action("Zoom +", () => plot.Zoom(1.35f)), Action("Zoom −", () => plot.Zoom(1 / 1.35f))), 0, 0);
        view.Controls.Add(plot, 0, 1);
        body.Controls.Add(view, 1, 0);
        layout.Controls.Add(body, 0, 2);
        Controls.Add(layout);

        connect.Click += (_, _) => ToggleConnection();
        demo.Click += (_, _) => ToggleDemo();
        anchorSetup.Click += (_, _) => LoadAnchors();
        copy.Click += (_, _) => CopyPosition();
        export.Click += (_, _) => ExportTracks();
        rovers.SelectedIndexChanged += (_, _) => { if (!updating) UpdateSelection(); };
        timer.Tick += (_, _) => Tick();
        FormClosed += (_, _) => { timer.Dispose(); feed?.Stop(); };
        RefreshPorts();
        timer.Start();
    }

    private static Label TextLabel(string text, int size) => new()
    {
        Text = text, AutoSize = true, ForeColor = TrackView.Ink, Font = new("Segoe UI", size),
        Margin = new(3, 6, 12, 6)
    };

    private static FlowLayoutPanel Flow(params Control[] children)
    {
        var panel = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Fill, WrapContents = false };
        panel.Controls.AddRange(children);
        return panel;
    }

    private static Button Action(string text, Action handler)
    {
        var button = new Button { Text = text, AutoSize = true };
        button.Click += (_, _) => handler();
        return button;
    }

    private static ListView Table(params (string Label, int Width)[] columns)
    {
        var list = new ListView { View = View.Details, FullRowSelect = true, MultiSelect = false,
            HideSelection = false, HeaderStyle = ColumnHeaderStyle.Nonclickable, Height = 178,
            Width = 475, BackColor = TrackView.Canvas, ForeColor = TrackView.Ink, Dock = DockStyle.Top };
        foreach (var (label, width) in columns)
            list.Columns.Add(label, width);
        return list;
    }

    private int Selected => rovers.SelectedItems.Count == 0 ? 0 : (int)rovers.SelectedItems[0].Tag!;

    private void RefreshPorts()
    {
        if (feed != null)
            return;
        var previous = ports.SelectedItem as string;
        ports.Items.Clear();
        ports.Items.AddRange(SerialPort.GetPortNames().Order().Cast<object>().ToArray());
        if (previous != null && ports.Items.Contains(previous))
            ports.SelectedItem = previous;
        else if (ports.Items.Count > 0)
            ports.SelectedIndex = 0;
        if (ports.Items.Count == 0)
            connection.Text = "No USB COM port found. Plug in the Base and click Refresh.";
    }

    private void ToggleConnection()
    {
        if (feed != null)
        {
            feed.Stop();
            feed = null;
            activePort = null;
            SetLink(false);
            connection.Text = "Disconnected · last display retained";
            connect.Text = "Connect";
            ports.Enabled = true;
            demo.Enabled = true;
            Render();
            return;
        }
        if (ports.SelectedItem is not string port)
        {
            RefreshPorts();
            return;
        }
        if (lastPort != port)
            fleet.Reset();
        lastPort = activePort = port;
        feed = new(port);
        feed.Start();
        connect.Text = "Disconnect";
        ports.Enabled = false;
        demo.Enabled = false;
        lastRead = 0;
        connectionEpoch = 0;
        Render();
    }

    private void ToggleDemo()
    {
        demoActive = !demoActive;
        fleet.Reset();
        linkUp = false;
        lastPort = null;
        demoStep = 0;
        demoAt = 0;
        demo.Text = demoActive ? "Exit demo" : "Demo";
        connect.Enabled = !demoActive;
        anchorSetup.Enabled = !demoActive;
        if (!demoActive)
            connection.Text = "Demo ended · connect the Base by USB.";
        Render();
    }

    private void SetLink(bool connected)
    {
        if (linkUp && !connected)
            fleet.Interrupt();
        linkUp = connected;
    }

    private void Tick()
    {
        long now = Stopwatch.GetTimestamp();
        if (demoActive)
        {
            if (demoAt != 0 && Stopwatch.GetElapsedTime(demoAt, now).TotalSeconds < 1.6)
                return;
            demoAt = now;
            DemoSnapshot();
            SetLink(true);
            connection.Text = "DEMO · simulated fleet and synthetic locations";
        }
        else if (feed != null)
        {
            if (feed.Take() is ReceivedSnapshot input)
            {
                if (connectionEpoch != input.Connection)
                    fleet.Interrupt();
                connectionEpoch = input.Connection;
                fleet.Apply(input.Snapshot, input.Received);
                lastRead = now;
            }
            SetLink(lastRead != 0 && Stopwatch.GetElapsedTime(lastRead, now).TotalSeconds < 7);
            string state = fleet.Latest?.State switch
            {
                BaseState.READY => "Base radio ready",
                BaseState.CONFIG => "Base needs provisioning",
                BaseState.RADIO => "Base radio fault",
                BaseState.STORE => "Base storage fault",
                _ => "Waiting for Base"
            };
            connection.Text = linkUp ? $"{activePort}  /  {state}" :
                $"{activePort}  /  {feed.Status} · requires EWS1 Base firmware";
        }
        else
            return;
        Render();
    }

    private void Render()
    {
        var snapshot = fleet.Latest;
        bool live = linkUp && snapshot?.State == BaseState.READY;
        if (snapshot != evaluatedSnapshot)
        {
            evaluatedSnapshot = snapshot;
            UpdateEstimates(snapshot);
        }
        int selected = Selected;
        var shown = snapshot?.Rovers.Where(r => r.Seen)
            .OrderByDescending(r => r.Alarm == Alarm.Sos).ThenBy(r => r.Id).ToArray() ?? [];
        int count = shown.Count(r => r.Alarm == Alarm.Sos);
        alert.Text = count > 0 ? $"{count} ACTIVE SOS{(live ? "" : " · LAST KNOWN")}" :
            live ? "NO ACTIVE SOS" : "BASE STATUS UNAVAILABLE";
        alert.ForeColor = count > 0 ? Color.Salmon : TrackView.Ink;

        var roverRows = shown.Select(r => (
            r.Id, r.Alarm == Alarm.Sos ? Color.Salmon : TrackView.Colors[r.Id - 1],
            new[] { r.Label, r.Alarm switch { Alarm.Sos when r.Restored => "SOS restored",
                Alarm.Sos => "SOS", Alarm.Cleared => "Cleared", _ => "—" },
                Heard(r.ContactAge, Fleet.RoverLate, live),
                r.Route == 0 ? "Direct" : $"Node {r.Route}",
                r.Fix == Fix.Valid && live && r.FixAge < Fleet.RoverLate ? "Recent fix" :
                live && estimates[r.Id - 1] != null ? "RF estimate" :
                r.HasPosition ? "Last known" : r.Fix == Fix.Missing ? "No fix" : "Unknown" })).ToArray();
        var nodeRows = snapshot?.Nodes.Where(n => n.HeartbeatAge >= 0 || n.ForwardAge >= 0 ||
                shown.Any(r => r.Route == n.Id)).Select(n => (
            n.Id, TrackView.Ink, new[] { $"Node {n.Id}",
                !live ? "Unknown" : n.HeartbeatAge is >= 0 and < Fleet.NodeLate ? "Checking in" :
                n.ForwardAge is >= 0 and < Fleet.NodeLate ? "Traffic heard" :
                n.HeartbeatAge < 0 && n.ForwardAge < 0 ? "Not heard" : "Overdue",
                Heard(n.HeartbeatAge, Fleet.NodeLate, live), Heard(n.ForwardAge, Fleet.NodeLate, live) }))
                .ToArray() ?? [];
        updating = true;
        UpdateRows(rovers, roverRows, selected);
        UpdateRows(nodes, nodeRows, 0);
        updating = false;
        UpdateSelection();
        export.Enabled = fleet.Tracks.Any(t => t.Count > 0) && !demoActive;
    }

    private static void UpdateRows(ListView list, (int Id, Color Color, string[] Values)[] rows, int selected)
    {
        list.BeginUpdate();
        try
        {
            for (int i = 0; i < rows.Length; i++)
            {
                var row = rows[i];
                if (i == list.Items.Count)
                    list.Items.Add(new ListViewItem(row.Values));
                var item = list.Items[i];
                item.Tag = row.Id;
                item.ForeColor = row.Color;
                for (int column = 0; column < row.Values.Length; column++)
                    if (item.SubItems[column].Text != row.Values[column])
                        item.SubItems[column].Text = row.Values[column];
                if (selected != 0)
                    item.Selected = row.Id == selected;
            }
            while (list.Items.Count > rows.Length)
                list.Items.RemoveAt(list.Items.Count - 1);
        }
        finally { list.EndUpdate(); }
    }

    private static string Heard(int age, int late, bool live) =>
        !live || age < 0 ? "—" : age >= late ? "Overdue" : $"{age} s ago";

    private Rover? SelectedRover() => Selected is int id and > 0 ? fleet.Latest?.Rovers[id - 1] : null;

    private void LoadAnchors()
    {
        using var dialog = new OpenFileDialog { Filter = "Anchor CSV (*.csv)|*.csv", CheckFileExists = true };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        try
        {
            using var reader = File.OpenText(dialog.FileName);
            anchors = SignalLocation.ReadAnchors(reader);
            configuredBase = demoActive ? 0 : fleet.Latest?.Base ?? 0;
            anchorSetup.Text = $"Anchors ({anchors.Length})";
            UpdateEstimates(fleet.Latest);
            Render();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or FormatException)
        {
            MessageBox.Show(this, ex.Message, "Anchor setup");
        }
    }

    private void UpdateEstimates(Snapshot? snapshot)
    {
        Array.Clear(estimates);
        var activeAnchors = demoActive ? DemoAnchors : anchors;
        if (snapshot?.State != BaseState.READY || activeAnchors.Length == 0)
            return;
        if (!demoActive && configuredBase != 0 && configuredBase != snapshot.Base)
        {
            anchors = [];
            configuredBase = 0;
            anchorSetup.Text = "Load anchors";
            return;
        }
        if (!demoActive)
            configuredBase = snapshot.Base;
        foreach (Rover rover in snapshot.Rovers)
            if (rover.Alarm == Alarm.Sos && rover.Fix == Fix.Missing &&
                snapshot.Signals[rover.Id - 1] is SignalEvidence evidence)
                estimates[rover.Id - 1] = SignalLocation.Locate(activeAnchors, evidence);
    }

    private void UpdateSelection()
    {
        var rover = SelectedRover();
        bool live = linkUp && fleet.Latest?.State == BaseState.READY;
        var estimate = live && rover != null ? estimates[rover.Id - 1] : null;
        copy.Enabled = (rover?.HasPosition == true || estimate != null) && !demoActive;
        copy.Text = estimate != null ? "Copy estimate" : "Copy GPS";
        details.Text = estimate != null
            ? $"RF estimate · {estimate.Position}\nSearch radius {estimate.Radius} m · {estimate.Anchors} anchors"
            : live && rover is { Alarm: Alarm.Sos, Fix: Fix.Missing }
                ? $"{rover.Label} · No GPS fix · {(anchors.Length == 0 ? "Load calibrated anchors" : "No usable RF estimate")}" +
                  (rover.HasPosition ? $"\nLast GPS · {rover.Position}" : "")
            : rover?.HasPosition == true
            ? $"{rover.Label}  ·  {rover.Position}  ·  {(live && rover.Fix == Fix.Valid && rover.FixAge < Fleet.RoverLate ? "recent fix" : "last known")}"
            : rover != null ? $"{rover.Label}  ·  No coordinates available"
            : "Select a rover to inspect its last known position.";
        plot.ShowFleet(Selected, live, estimates);
    }

    private void CopyPosition()
    {
        if (SelectedRover() is not Rover rover)
            return;
        SignalEstimate? estimate = linkUp && fleet.Latest?.State == BaseState.READY ? estimates[rover.Id - 1] : null;
        if (estimate == null && !rover.HasPosition)
            return;
        string position = estimate == null ? rover.Position.ToString() :
            $"RF estimate: {estimate.Position} (search radius {estimate.Radius} m)";
        try { Clipboard.SetText(position); }
        catch (System.Runtime.InteropServices.ExternalException)
        {
            MessageBox.Show(this, "The clipboard is busy. Try again.", Text);
        }
    }

    private void ExportTracks()
    {
        using var dialog = new SaveFileDialog { Filter = "GeoJSON (*.geojson)|*.geojson", FileName = "ews-rover-tracks.geojson" };
        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;
        try
        {
            var features = fleet.Tracks.SelectMany((track, i) => track.GroupBy(p => p.Leg).Select(leg =>
            {
                var coordinates = leg.Select(p => new[] { p.Position.Lon / 1e5, p.Position.Lat / 1e5 }).ToArray();
                return new
                {
                    type = "Feature",
                    properties = new { rover_id = i + 1, leg = leg.Key,
                        base_receipt_utc = leg.Select(p => p.BaseReceipt).ToArray() },
                    geometry = new { type = coordinates.Length == 1 ? "Point" : "LineString",
                        coordinates = coordinates.Length == 1 ? (object)coordinates[0] : coordinates }
                };
            })).ToArray();
            File.WriteAllText(dialog.FileName, JsonSerializer.Serialize(new { type = "FeatureCollection", features }));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, "Unable to save tracks. Check the selected folder.", Text);
        }
    }

    internal bool SmokeTest()
    {
        ToggleDemo();
        for (int i = 0; i < 8; i++)
            DemoSnapshot();
        SetLink(true);
        Render();
        bool gps = rovers.Items.Count == 2 && nodes.Items.Count == 1 &&
                   fleet.Tracks[0].Count == 8 && alert.Text == "1 ACTIVE SOS";
        for (int i = 0; i < 3; i++)
            DemoSnapshot();
        Render();
        using var preview = new Bitmap(plot.Width, plot.Height);
        plot.DrawToBitmap(preview, plot.ClientRectangle);
        return gps && estimates[0] != null && fleet.Tracks[0].Count == 10 &&
               rovers.Items[0].SubItems[4].Text == "RF estimate";
    }

    private void DemoSnapshot()
    {
        int step = demoStep++;
        bool noGps = step % 24 is >= 10 and < 17;
        var frame = new StringBuilder($"EWS1,B,1,1,{step},READY\n");
        for (int i = 1; i <= 8; i++)
        {
            if (i is 1 or 2)
            {
                int age = i == 1 && noGps ? step % 24 - 9 : 0;
                int lastFix = step - age;
                int angle = lastFix % 100;
                int alarm = i == 1 && step % 24 is >= 6 and < 17 ? 1 : i == 1 && step % 24 >= 17 ? 2 : 0;
                int lat = 1250000 + (int)(55 * Math.Sin(angle / 6.0)) + i * 35;
                int lon = 12350000 + (int)(65 * Math.Cos(angle / 6.0)) + i * 60;
                frame.AppendLine($"EWS1,R,{i},{i - 1},{(i == 1 ? "ALPHA" : "BRAVO")},{alarm},0,0,{(age == 0 ? 2 : 1)},{age},{lat},{lon},{lastFix + 1},{(lastFix + 7) / 24 + 1},0");
            }
            else
                frame.AppendLine($"EWS1,R,{i},0,,0,0,-1,0,-1,0,0,0,0,0");
        }
        for (int i = 1; i <= 8; i++)
            frame.AppendLine($"EWS1,N,{i},{(i == 1 ? step % 8 : -1)},{(i == 1 ? 0 : -1)}");
        if (noGps)
            frame.AppendLine("EWSL,A,1,10,-835,-835,-853,0,0,0,0,0,0");
        frame.AppendLine("EWS1,E");
        new TelemetryReader().Feed(frame.ToString(), snapshot => fleet.Apply(snapshot, DateTimeOffset.UtcNow));
    }
}
