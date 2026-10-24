# Desk display: show the fullest disk of a computer running the System
# monitor on every desk display in this account. A report with disks in it
# ("/ used %", "C: used %") is one; the line goes out when it differs from
# the last one sent (with two computers, their lines take turns), and to a
# display that has just started: its first report is tagged "hello".
# Nothing to type in: the displays are found by their demo's name, the
# MicroPython desk display's or the Arduino one's.

DISPLAYS = ["demo-cyd-desk-display", "demo-cyd-desk-display-arduino"]
FRESH = 10                               # minutes: an older line is not given to a display that starts

def routine(event, state, mem):
    hw = event["hw_id"]
    minute = event["ts_ms"] // 60000
    if event["tag"] == "hello":          # a display just started: give it the last line
        if minute - mem.get("minute", 0) <= FRESH and hw in devices_from(DISPLAYS):
            send(hw, "info:" + mem["line"])
        return

    # The disk closest to full in this report, if it has disks at all.
    disk, used = None, -1
    for name, value in event["fields"].items():
        if name.endswith(" used %") and value > used:
            disk, used = name[:-len(" used %")], value
    if disk == None:
        return                           # not a report about disks

    line = "%d%% full: %s %s" % (int(used + 0.5), event["fields"].get("host", "?"), disk)
    line = line[:40]                     # short; the screen shows "87% raspberrypi /", 20 letters at most
    if line != mem.get("line"):
        for display in devices_from(DISPLAYS):
            send(display, "info:" + line)
    mem["line"] = line                   # the last line, and the minute it was last true
    mem["minute"] = minute
